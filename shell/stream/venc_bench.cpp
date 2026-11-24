// НаомиФон — замер аппаратного видеокодера тем же путём, что у трансляции (docs/STREAM.md, «Замеры»):
// видеочип рисует пробный кадр (движущиеся полосы и мелкий узор), пакует его в NV12 прямо в буфер кодера
// (nv12_pack), кодер сжимает. Считает время упаковки, время сжатия кадра (от кадра в кодер до сжатого) и
// размер; пишет поток в файл — проверить на Mac (ffprobe/ffmpeg).
//
//   naomi-venc-bench [--size 720x1560] [--fps 90] [--mbps 60] [--maxqp 22] [--frames 450] [--hevc] [--flat]
//                    [--rate к/с] [--ll] [--ctrl 0xID=число] [--out /tmp/bench.h264]
// --flat — кадры подаются без пауз (сколько кодер успевает), иначе — ровно с частотой кадров.
#include "gralloc_buffer.h"
#include "nv12_pack.h"
#include "venus_encoder.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <poll.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char kPatternVs[] = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
// похоже на интерфейс: тёмный фон, светлые «строки текста», янтарная полоса едет вниз
const char kPatternFs[] = R"(#version 300 es
precision highp float;
uniform float t;
uniform vec2 size;
out vec4 o;
void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 c = vec3(0.04);
    float row = floor(p.y / 40.0);
    float inRow = mod(p.y, 40.0);
    if (inRow > 12.0 && inRow < 26.0 && p.x > 40.0 && p.x < 40.0 + fract(sin(row * 12.9898) * 43758.5) * (size.x - 80.0))
        c = vec3(0.85) * step(0.5, fract(p.x / 7.0 + row * 0.37));
    float bar = mod(t * 300.0, size.y);
    if (abs(p.y - (size.y - bar)) < 60.0) c = vec3(1.0, 0.69, 0.22);
    if (p.x < 6.0 || p.y < 6.0) c = vec3(1.0, 0.0, 0.0);   // метка «левый нижний угол GL»
    o = vec4(c, 1.0);
})";

GLuint program(const char *vs, const char *fs)
{
    auto sh = [](GLenum type, const char *src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

double pct(std::vector<double> v, double q)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

} // namespace

int main(int argc, char **argv)
{
    VenusEncoder::Settings s;
    int frames = 450;
    bool flat = false;
    const char *out = "/tmp/naomi-bench.h264";
    for (int i = 1; i < argc; ++i) {
        const auto arg = [&](const char *name) { return !strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--size"))
            sscanf(argv[++i], "%dx%d", &s.width, &s.height);
        else if (arg("--fps"))
            s.fps = atoi(argv[++i]);
        else if (arg("--mbps"))
            s.bitrate = int(atof(argv[++i]) * 1e6);
        else if (arg("--maxqp"))   // потолок квантования (0 — без него)
            s.maxQp = atoi(argv[++i]);
        else if (arg("--frames"))
            frames = atoi(argv[++i]);
        else if (arg("--out"))
            out = argv[++i];
        else if (!strcmp(argv[i], "--hevc"))
            s.hevc = true;
        else if (!strcmp(argv[i], "--ll"))   // режим малой задержки кодера
            s.lowLatency = true;
        else if (!strcmp(argv[i], "--flat"))
            flat = true;
        else if (arg("--rate"))   // «скорость работы» кодера, к/с; 0 — наибольшая
            s.operatingRate = atoi(argv[++i]);
        else if (arg("--ctrl")) {   // разведка: --ctrl 0x00992077=15728640 — своя настройка кодера
            unsigned id = 0;
            int value = 0;
            sscanf(argv[++i], "%x=%d", &id, &value);
            s.extra.push_back({id, value});
        }
    }

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, nullptr, nullptr);
    const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, 0x40 /* ES3 */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint pbAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (!ctx || !eglMakeCurrent(dpy, pb, pb, ctx)) {
        fprintf(stderr, "EGL: контекст не создан\n");
        return 1;
    }

    VenusEncoder enc;
    if (!enc.open(s) || !enc.prepareQueues(4, 4))
        return 1;
    const auto queues = enc.queues();
    const auto L = queues.input;
    fprintf(stderr, "кодер: %s %dx%d, %d к/с, %.1f Мбит/с; NV12: строка %d, %d+%d строк, %zu байт; сжатый ≤ %zu\n",
            s.hevc ? "HEVC" : "H.264", s.width, s.height, s.fps, s.bitrate / 1e6, L.stride, L.yScan, L.uvScan,
            L.size, queues.packet);

    // источник — текстура RGBA размером с кадр (как копия кадра оболочки)
    GLuint srcTex, srcFbo;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, s.width, s.height);
    glGenFramebuffers(1, &srcFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
    const GLuint pattern = program(kPatternVs, kPatternFs);
    GLuint vao;
    glGenVertexArrays(1, &vao);

    std::vector<GrallocBuffer> in(queues.inputs), outBufs(queues.outputs);
    std::vector<GrallocBuffer> inExtra(queues.inputExtra ? queues.inputs : 0),
        outExtra(queues.packetExtra ? queues.outputs : 0);
    std::vector<VenusEncoder::Buffer> encIn, encOut;
    for (unsigned i = 0; i < queues.inputs; ++i) {
        auto &b = in[i];
        if (!grallocAllocateLinear(b, L.size, L.stride / 4,
                                   kGrallocHwRender | kGrallocHwTexture | kGrallocHwVideoEncoder | kGrallocSwReadRarely)
            || !grallocBindGl(b, dpy))
            return 1;
        if (b.size < L.size) {
            fprintf(stderr, "gralloc: %zu байт — меньше кадра кодера\n", b.size);
            return 1;
        }
        encIn.push_back({b.fd, b.size, nullptr});
        if (!inExtra.empty()) {
            if (!grallocAllocateExtra(inExtra[i], queues.inputExtra))
                return 1;
            encIn.back().extraFd = inExtra[i].fd;
            encIn.back().extraSize = inExtra[i].size;
        }
    }
    for (unsigned i = 0; i < queues.outputs; ++i) {
        auto &b = outBufs[i];
        if (!grallocAllocate(b, int(queues.packet), 1, kGrallocBlob,
                             kGrallocSwReadOften | kGrallocHwVideoEncoder)
            || !grallocMapForCpu(b))
            return 1;
        encOut.push_back({b.fd, b.size, b.map});
        if (!outExtra.empty()) {
            if (!grallocAllocateExtra(outExtra[i], queues.packetExtra))
                return 1;
            encOut.back().extraFd = outExtra[i].fd;
            encOut.back().extraSize = outExtra[i].size;
        }
    }
    if (!enc.start(encIn, encOut))
        return 1;

    Nv12Packer packer;
    if (!packer.init(s.width, s.height, L.stride, L.yScan, L.size, in[0].stride))
        return 1;

    FILE *f = fopen(out, "wb");
    if (!f) {
        perror(out);
        return 1;
    }
    std::vector<bool> free(queues.inputs, true);
    std::map<int64_t, int64_t> queuedAt;   // метка кадра → когда отдан кодеру
    std::vector<double> packMs, encMs;
    size_t bytes = 0, keys = 0;
    std::atomic<size_t> done{0};
    const int64_t period = 1000000 / s.fps, t0 = nowUs();
    int64_t next = t0;
    int sent = 0;

    // Сжатое забирает свой поток, как в трансляции: главный в это время рисует и пакует следующий кадр (ждёт
    // видеочип) — заберёт он пакет сам, время сжатия вышло бы длиной в период кадра (так и было до 17 ноября 2026:
    // «сжатие тянется на весь период» — ошибка замера)
    std::mutex lock;
    std::atomic<bool> stop{false};
    auto drain = [&](int timeoutMs) {
        pollfd p{enc.pollFd(), POLLIN | POLLOUT, 0};
        poll(&p, 1, timeoutMs);
        const int64_t now = nowUs();
        std::lock_guard<std::mutex> g(lock);
        VenusEncoder::Packet pkt;
        while (enc.takePacket(pkt)) {
            if (auto it = queuedAt.find(pkt.timestampUs); it != queuedAt.end()) {
                encMs.push_back((now - it->second) / 1000.0);
                queuedAt.erase(it);
                ++done;
            }
            bytes += pkt.data.size();
            keys += VenusEncoder::hasKeyNal(pkt.data, s.hevc);
            fwrite(pkt.data.data(), 1, pkt.data.size(), f);
        }
        for (int i; (i = enc.takeFreeInput()) >= 0;)
            free[i] = true;
    };
    std::thread taker([&] {
        while (!stop)
            drain(2);
    });
    auto freeSlot = [&] {
        std::lock_guard<std::mutex> g(lock);
        return int(std::find(free.begin(), free.end(), true) - free.begin());
    };

    bool ok = true;
    while (sent < frames) {
        if (!flat) {   // ровно с частотой кадров
            while (nowUs() < next)
                usleep(useconds_t(std::clamp<int64_t>(next - nowUs(), 50, 2000)));
            next += period;
        }
        int slot = -1;
        const int64_t deadline = nowUs() + 2000000;
        while ((slot = freeSlot()) >= int(queues.inputs) && nowUs() < deadline)
            usleep(200);
        if (slot >= int(queues.inputs)) {
            fprintf(stderr, "venus: кодер не вернул буфер за 2 с\n");
            ok = false;
            break;
        }
        {
            std::lock_guard<std::mutex> g(lock);
            free[slot] = false;
        }

        const int64_t ts = nowUs();
        glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
        glViewport(0, 0, s.width, s.height);
        glUseProgram(pattern);
        glUniform1f(glGetUniformLocation(pattern, "t"), sent / float(s.fps));
        glUniform2f(glGetUniformLocation(pattern, "size"), s.width, s.height);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        const int64_t packStart = nowUs();
        packer.pack(srcTex, false, in[slot].fbo);
        glFinish();   // в оболочке — fence; здесь просто ждём видеочип
        packMs.push_back((nowUs() - packStart) / 1000.0);
        std::lock_guard<std::mutex> g(lock);
        const int64_t queued = nowUs();
        if (!enc.encode(slot, ts)) {
            ok = false;
            break;
        }
        queuedAt[ts] = queued;
        ++sent;
    }
    for (int i = 0; i < 100 && done < size_t(sent); ++i)
        usleep(10000);
    stop = true;
    taker.join();
    const double seconds = (nowUs() - t0) / 1e6;
    const size_t encoded = done.load();
    fclose(f);
    printf("{\"codec\": \"%s\", \"size\": \"%dx%d\", \"fps_target\": %d, \"paced\": %s, \"frames\": %d, "
           "\"encoded\": %zu, \"seconds\": %.2f, \"fps\": %.1f, \"pack_ms\": {\"median\": %.2f, \"p95\": %.2f}, "
           "\"encode_ms\": {\"median\": %.2f, \"p95\": %.2f, \"max\": %.2f}, \"mbps\": %.2f, \"key_packets\": %zu, "
           "\"file\": \"%s\"}\n",
           s.hevc ? "hevc" : "h264", s.width, s.height, s.fps, flat ? "false" : "true", sent, encoded, seconds,
           encoded / seconds, pct(packMs, 0.5), pct(packMs, 0.95), pct(encMs, 0.5), pct(encMs, 0.95),
           pct(encMs, 1.0), bytes * 8 / seconds / 1e6, keys, out);
    enc.close();
    for (auto *pool : {&in, &outBufs, &inExtra, &outExtra})
        for (auto &b : *pool)
            grallocFree(b, dpy);
    return ok && encoded == size_t(frames) ? 0 : 1;
}
