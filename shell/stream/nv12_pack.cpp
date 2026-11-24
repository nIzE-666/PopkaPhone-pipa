#include "nv12_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char kVertex[] = R"(#version 300 es
void main() {
    // треугольник на весь кадр
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Пиксель цели (x, y) — байты y·pitch·4 + x·4 … +3 буфера; в NV12 это строка b / nvStride, столбец b % nvStride (4 байта
// пикселя — в одной строке NV12: nvStride кратна 512). У EGLImage первая строка памяти — y = 0.
const char kFragment[] = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D src;
uniform ivec2 size;      // кадр потока
uniform int nvStride;    // строка NV12, байт
uniform int yScan;       // начало цветности, строк NV12
uniform int pitch;       // строка цели, байт
uniform bool flipY;
uniform int rotation;
out vec4 o;

vec3 rgb(vec2 px) {      // px — точка кадра потока в пикселях (центр пикселя — .5)
    vec2 uv = px / vec2(size);
    if (rotation == 90) uv = vec2(1.0 - uv.y, uv.x);
    if (rotation == 180) uv = 1.0 - uv;
    if (rotation == 270) uv = vec2(uv.y, 1.0 - uv.x);
    if (flipY) uv.y = 1.0 - uv.y;
    return texture(src, uv).rgb;
}
// BT.709, узкий диапазон, байты / 255
float luma(vec3 c) { return (16.0 + 219.0 * dot(c, vec3(0.2126, 0.7152, 0.0722))) / 255.0; }
vec2 chroma(vec3 c) {
    return vec2(128.0 + 224.0 * dot(c, vec3(-0.11457, -0.38543, 0.5)),
                128.0 + 224.0 * dot(c, vec3(0.5, -0.45415, -0.04585))) / 255.0;
}

void main() {
    int b = int(gl_FragCoord.y) * pitch + int(gl_FragCoord.x) * 4;
    int row = b / nvStride;
    int px = b - row * nvStride;
    if (row < size.y) {                       // яркость: 4 пикселя строки
        float y = float(row) + 0.5;
        o = vec4(px < size.x ? luma(rgb(vec2(float(px) + 0.5, y))) : 0.0,
                 px + 1 < size.x ? luma(rgb(vec2(float(px) + 1.5, y))) : 0.0,
                 px + 2 < size.x ? luma(rgb(vec2(float(px) + 2.5, y))) : 0.0,
                 px + 3 < size.x ? luma(rgb(vec2(float(px) + 3.5, y))) : 0.0);
    } else if (row >= yScan && row < yScan + size.y / 2) {   // цветность: две пары U, V на блоки 2 × 2
        // середина блока 2 × 2: линейная выборка усредняет четыре пикселя
        float y = float((row - yScan) * 2 + 1);
        o = vec4(px < size.x ? chroma(rgb(vec2(float(px) + 1.0, y))) : vec2(0.5),
                 px + 2 < size.x ? chroma(rgb(vec2(float(px) + 3.0, y))) : vec2(0.5));
    } else {                                  // выравнивание кодера
        o = vec4(0.0);
    }
}
)";

// Быстрый путь (строка цели = строка NV12): точка цели (x, y) — байты 4x … 4x+3 строки y
const char kCommon[] = R"(#version 300 es
precision highp float;
uniform highp sampler2D src;
uniform vec2 inv;        // 1 / размер кадра потока
uniform float yScan;     // начало цветности, строк
uniform bool flipY;
uniform int rotation;
out vec4 o;
vec3 rgb(float x, float y) {   // точка кадра потока в пикселях (центр пикселя — .5)
    vec2 uv = vec2(x, y) * inv;
    if (rotation == 90) uv = vec2(1.0 - uv.y, uv.x);
    if (rotation == 180) uv = 1.0 - uv;
    if (rotation == 270) uv = vec2(uv.y, 1.0 - uv.x);
    if (flipY) uv.y = 1.0 - uv.y;
    return texture(src, uv).rgb;
}
float luma(vec3 c) { return (16.0 + 219.0 * dot(c, vec3(0.2126, 0.7152, 0.0722))) / 255.0; }
vec2 chroma(vec3 c) {
    return vec2(128.0 + 224.0 * dot(c, vec3(-0.11457, -0.38543, 0.5)),
                128.0 + 224.0 * dot(c, vec3(0.5, -0.45415, -0.04585))) / 255.0;
}
)";
const char kLuma[] = R"(
void main() {
    float px = floor(gl_FragCoord.x) * 4.0, y = floor(gl_FragCoord.y) + 0.5;
    o = vec4(luma(rgb(px + 0.5, y)), luma(rgb(px + 1.5, y)), luma(rgb(px + 2.5, y)), luma(rgb(px + 3.5, y)));
}
)";
const char kChroma[] = R"(
void main() {
    // середина блока 2 × 2: линейная выборка усредняет четыре пикселя
    float px = floor(gl_FragCoord.x) * 4.0, y = (floor(gl_FragCoord.y) - yScan) * 2.0 + 1.0;
    o = vec4(chroma(rgb(px + 1.0, y)), chroma(rgb(px + 3.0, y)));
}
)";

GLuint compile(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "nv12: шейдер не собран: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

// программа из общего вершинного шейдера и фрагментного из частей; 0 — не собрана
static GLuint build(const char *fragment, const char *tail = nullptr)
{
    const std::string source = tail ? std::string(fragment) + tail : std::string(fragment);
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, source.c_str());
    if (!vs || !fs)
        return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "nv12: программа не собрана\n");
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool Nv12Packer::init(int w, int h, int nvStride, int yScan, size_t size, int pitch, int rotation)
{
    m_pitch = pitch;
    m_rows = int((size + size_t(pitch) * 4 - 1) / (size_t(pitch) * 4));   // строк цели под весь кадр NV12
    m_w = w;
    m_h = h;
    m_yScan = yScan;
    m_program = build(kFragment);
    if (!m_program)
        return false;
    const char *mode = getenv("NAOMI_STREAM_PACK");
    if (pitch * 4 == nvStride && !(mode && !strcmp(mode, "old"))) {
        m_luma = build(kCommon, kLuma);
        m_chroma = build(kCommon, kChroma);
        if (!m_luma || !m_chroma) {
            m_luma = m_chroma = 0;   // остаётся общий шейдер
        } else {
            for (GLuint program : {m_luma, m_chroma}) {
                glUseProgram(program);
                glUniform1i(glGetUniformLocation(program, "src"), 0);
                glUniform2f(glGetUniformLocation(program, "inv"), 1.0f / float(w), 1.0f / float(h));
                glUniform1f(glGetUniformLocation(program, "yScan"), float(yScan));
                glUniform1i(glGetUniformLocation(program, "rotation"), rotation);
            }
            m_lumaFlip = glGetUniformLocation(m_luma, "flipY");
            m_chromaFlip = glGetUniformLocation(m_chroma, "flipY");
        }
    }
    glUseProgram(m_program);
    glUniform1i(glGetUniformLocation(m_program, "src"), 0);
    glUniform2i(glGetUniformLocation(m_program, "size"), w, h);
    glUniform1i(glGetUniformLocation(m_program, "nvStride"), nvStride);
    glUniform1i(glGetUniformLocation(m_program, "yScan"), yScan);
    glUniform1i(glGetUniformLocation(m_program, "pitch"), pitch * 4);
    glUniform1i(glGetUniformLocation(m_program, "rotation"), rotation);
    m_uFlip = glGetUniformLocation(m_program, "flipY");
    glUseProgram(0);
    glGenVertexArrays(1, &m_vao);
    return true;
}

void Nv12Packer::pack(GLuint src, bool flipY, GLuint dstFbo)
{
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, m_pitch, m_rows);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(m_program);
    glUniform1i(m_uFlip, flipY ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindVertexArray(m_vao);
    if (m_luma) {
        // буфер впервые — очистить целиком (выравнивание потом не трогаем); дальше прежнее содержимое не нужно
        if (std::find(m_cleared.begin(), m_cleared.end(), dstFbo) == m_cleared.end()) {
            m_cleared.push_back(dstFbo);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            const GLenum color = GL_COLOR_ATTACHMENT0;
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &color);
        }
        const int columns = (m_w + 3) / 4;
        glEnable(GL_SCISSOR_TEST);
        glUseProgram(m_luma);
        glUniform1i(m_lumaFlip, flipY ? 1 : 0);
        glScissor(0, 0, columns, m_h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(m_chroma);
        glUniform1i(m_chromaFlip, flipY ? 1 : 0);
        glScissor(0, m_yScan, columns, m_h / 2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_SCISSOR_TEST);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

void Nv12Packer::release()
{
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    for (GLuint program : {m_program, m_luma, m_chroma})
        if (program)
            glDeleteProgram(program);
    m_vao = m_program = m_luma = m_chroma = 0;
    m_cleared.clear();
}
