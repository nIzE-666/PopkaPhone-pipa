#include "venus_encoder.h"
#include "device_profile.h"

#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kDevice = "/dev/video/venus_enc";
constexpr uint32_t kOut = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;    // вход кодера: сырые кадры
constexpr uint32_t kCap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;   // выход: сжатое
// msm_vidc: номер dma-buf и смещение — в planes[].reserved[0] и [1]
constexpr int kReservedFd = 0, kReservedOffset = 1;
// своя настройка Qualcomm (V4L2_CID_MPEG_MSM_VIDC_BASE + 0x35): «скорость работы», к/с × 65536 — по ней драйвер
// выбирает частоты кодера: чем она выше, тем быстрее сжатие; предел — VenusEncoder::maxOperatingRate
constexpr uint32_t kCidOperatingRate = 0x00992035;
// своя настройка Qualcomm (V4L2_CID_MPEG_MSM_VIDC_BASE + 0x38): режим малой задержки, 0 / 1. Имён настройки драйвер
// не называет — найдена перебором: из 19 настроек с двумя-тремя значениями время сжатия меняет только она
constexpr uint32_t kCidLowLatency = 0x00992038;
// своя настройка Qualcomm (V4L2_CID_MPEG_VIDC_VENC_BITRATE_SAVINGS, BASE + 131): «экономия битов» — кодер сам решает,
// где перемену «не видно», и такие блоки не пишет: копирует из прошлого кадра. По умолчанию включена (3: и 8, и 10
// бит). На столе это текст движущегося окна — буквы двоятся и крошатся, вокруг цветные точки (docs/STREAM.md,
// «Экономия битов»); 0 — выключена
constexpr uint32_t kCidBitrateSavings = 0x00992083;

int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

int alignUp(int v, int a) { return (v + a - 1) / a * a; }

void setPlane(v4l2_plane &plane, int fd, size_t size, size_t used)
{
    plane.m.userptr = static_cast<unsigned long>(fd);
    plane.length = uint32_t(size);
    plane.bytesused = uint32_t(used);
    plane.reserved[kReservedFd] = uint32_t(fd);
    plane.reserved[kReservedOffset] = 0;
}

bool fits(const VenusEncoder::Buffer &b, size_t size, size_t extra)
{
    return b.fd >= 0 && b.size >= size && b.size <= UINT32_MAX
           && (!extra || (b.extraFd >= 0 && b.extraSize >= extra && b.extraSize <= UINT32_MAX));
}

bool hasFormat(int fd, uint32_t type, uint32_t fourcc)
{
    v4l2_fmtdesc format{};
    format.type = type;
    for (; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index)
        if (format.pixelformat == fourcc)
            return true;
    return false;
}

int openPipaEncoder(uint32_t codec)
{
    for (const char *node : {"/dev/video33", kDevice, "/dev/video32"}) {
        const int fd = ::open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && memcmp(cap.driver, "msm_vidc", 8) == 0
            && hasFormat(fd, kOut, V4L2_PIX_FMT_NV12) && hasFormat(fd, kCap, codec)) {
            fprintf(stderr, "venus: pipa, кодер %s\n", node);
            return fd;
        }
        ::close(fd);
    }
    fprintf(stderr, "venus: pipa: кодер msm_vidc с NV12 и выбранным кодеком не найден или недоступен\n");
    return -1;
}

// кодер пишет мимо кэша процессора: перед чтением — сброс кэша по dma-buf
void cpuRead(int fd, bool begin)
{
    dma_buf_sync s{__u64(DMA_BUF_SYNC_READ | (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END))};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

} // namespace

VenusEncoder::~VenusEncoder()
{
    close();
}

bool VenusEncoder::setControl(uint32_t id, int32_t value, const char *name)
{
    v4l2_control c{id, value};
    if (xioctl(m_fd, VIDIOC_S_CTRL, &c) < 0) {
        fprintf(stderr, "venus: настройка «%s» = %d: %s\n", name, value, strerror(errno));
        return false;
    }
    return true;
}

bool VenusEncoder::setVendorControl(uint32_t id, int32_t value, const char *name)
{
    if (m_kona) {
        v4l2_queryctrl q{};
        q.id = id;
        if (xioctl(m_fd, VIDIOC_QUERYCTRL, &q) < 0
            || (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_INACTIVE))
            || value < q.minimum || value > q.maximum) {
            fprintf(stderr, "venus: настройка «%s» (0x%x) = %d недоступна\n", name, id, value);
            return false;
        }
    }
    return setControl(id, value, name);
}

bool VenusEncoder::open(const Settings &s)
{
    close();
    if (s.width < 2 || s.height < 2 || s.width > 8192 || s.height > 8192 || s.width % 2 || s.height % 2 || s.fps < 1)
        return false;
    m_kona = DeviceProfile::current().tablet;
    const uint32_t codec = s.hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    m_fd = m_kona ? openPipaEncoder(codec) : ::open(kDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        if (!m_kona)
            fprintf(stderr, "venus: %s: %s\n", kDevice, strerror(errno));
        return false;
    }
    // сначала кодек (выход): без него драйвер не знает размеров
    v4l2_format f{};
    f.type = kCap;
    f.fmt.pix_mp.width = s.width;
    f.fmt.pix_mp.height = s.height;
    f.fmt.pix_mp.pixelformat = codec;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(m_fd, VIDIOC_S_FMT, &f) < 0) {
        fprintf(stderr, "venus: формат выхода: %s\n", strerror(errno));
        close();
        return false;
    }
    v4l2_format captureFormat = f;

    f = {};
    f.type = kOut;
    f.fmt.pix_mp.width = s.width;
    f.fmt.pix_mp.height = s.height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(m_fd, VIDIOC_S_FMT, &f) < 0) {
        fprintf(stderr, "venus: формат входа: %s\n", strerror(errno));
        close();
        return false;
    }

    // B-кадров нет (по умолчанию 0): декодеру не нужно переставлять кадры — нет задержки на перестановку.
    // Профиль Constrained Baseline: и декодер Mac (VideoToolbox в Chromium) не копит кадры «на всякий случай».
    bool ok = true;
    if (!s.hevc) {
        ok &= setControl(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
                         "профиль");
        ok &= setControl(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_5_1, "уровень");
    }
    ok &= setControl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "режим битрейта");
    ok &= setControl(V4L2_CID_MPEG_VIDEO_BITRATE, s.bitrate, "битрейт");
    // Потолок квантования. У Qualcomm он один на H.264 и HEVC (номер — HEVC_MAX_QP) и упакован: QP опорного,
    // P- и B-кадров — по байту. Без него кодер на резкой смене всего кадра (окно появилось, анимация) уходил в QP
    // до 51 — кадр распадался на блоки; с ним кадр просто крупнее (замер 13 ноября 2026: docs/STREAM.md)
    if (s.maxQp > 0)
        ok &= setControl(V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, s.maxQp | s.maxQp << 8 | s.maxQp << 16, "потолок QP");
    // опорный кадр — только по просьбе (новый зритель): связь надёжная (TCP), кадры не теряются
    ok &= setControl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, 268435455, "группа кадров");
    setVendorControl(kCidBitrateSavings, 0, "экономия битов");
    if (s.lowLatency)
        setVendorControl(kCidLowLatency, 1, "малая задержка");
    for (auto [id, value] : s.extra) {
        if (id == kCidOperatingRate || id == kCidLowLatency || id == kCidBitrateSavings)
            setVendorControl(id, value, "добавочная");
        else
            setControl(id, value, "добавочная");
    }
    setOperatingRate(s.operatingRate < 0 ? s.fps : s.operatingRate);
    // Размер CAPTURE зависит от кодека и режима битрейта, а число плоскостей — от настроек extradata.
    if (m_kona && (xioctl(m_fd, VIDIOC_G_FMT, &captureFormat) < 0 || xioctl(m_fd, VIDIOC_G_FMT, &f) < 0)) {
        fprintf(stderr, "venus: форматы после настройки: %s\n", strerror(errno));
        ok = false;
    }
    const auto &raw = f.fmt.pix_mp;
    const auto &encoded = captureFormat.fmt.pix_mp;
    const auto layout = nv12Layout(s.width, s.height);
    const auto validPlanes = [](const auto &format) {
        return format.plane_fmt[0].sizeimage && (format.num_planes == 1
               || (format.num_planes == 2 && format.plane_fmt[1].sizeimage));
    };
    if (!validPlanes(raw) || !validPlanes(encoded) || encoded.pixelformat != codec
        || raw.pixelformat != V4L2_PIX_FMT_NV12 || int(raw.width) != s.width || int(raw.height) != s.height
        || int(raw.plane_fmt[0].bytesperline) != layout.stride || raw.plane_fmt[0].sizeimage != layout.size) {
        fprintf(stderr, "venus: раскладка %dx%d не подходит: NV12 плоскостей %u, строка %u, буфер %u, CAPTURE %u\n",
                s.width, s.height, raw.num_planes, raw.plane_fmt[0].bytesperline, raw.plane_fmt[0].sizeimage,
                encoded.num_planes);
        ok = false;
    }
    if (!ok) {
        close();
        return false;
    }
    m_queues = {layout, raw.num_planes == 2 ? raw.plane_fmt[1].sizeimage : 0,
                encoded.plane_fmt[0].sizeimage, encoded.num_planes == 2 ? encoded.plane_fmt[1].sizeimage : 0};
    return true;
}

bool VenusEncoder::queueOutput(int index)
{
    v4l2_plane planes[2]{};
    const auto &buffer = m_outputs[index];
    setPlane(planes[0], buffer.fd, buffer.size, 0);
    if (m_queues.packetExtra)
        setPlane(planes[1], buffer.extraFd, m_queues.packetExtra, 0);
    v4l2_buffer b{};
    b.type = kCap;
    b.memory = V4L2_MEMORY_USERPTR;
    b.index = uint32_t(index);
    b.m.planes = planes;
    b.length = m_queues.packetExtra ? 2 : 1;
    if (xioctl(m_fd, VIDIOC_QBUF, &b) < 0) {
        fprintf(stderr, "venus: выходной буфер %d: %s\n", index, strerror(errno));
        return false;
    }
    return true;
}

bool VenusEncoder::prepareQueues(unsigned inputs, unsigned outputs)
{
    if (m_fd < 0 || m_streaming || !inputs || !outputs)
        return false;
    const auto request = [&](uint32_t type, unsigned count) -> unsigned {
        v4l2_requestbuffers r{};
        r.count = count;
        r.type = type;
        r.memory = V4L2_MEMORY_USERPTR;
        if (count > VIDEO_MAX_FRAME || xioctl(m_fd, VIDIOC_REQBUFS, &r) < 0 || r.count < count || r.count > VIDEO_MAX_FRAME) {
            fprintf(stderr, "venus: REQBUFS (%s): запрошено %u, выдано %u\n", type == kOut ? "вход" : "выход",
                    count, r.count);
            return 0;
        }
        return r.count;
    };
    if (m_kona) {
        // На ветке Kona минимальную очередь сообщает также число буферов, нужных firmware.
        const auto minimum = [this](uint32_t id) -> unsigned {
            v4l2_control ctrl{id, 0};
            return xioctl(m_fd, VIDIOC_G_CTRL, &ctrl) == 0 && ctrl.value > 0 ? unsigned(ctrl.value) : 0;
        };
        inputs = std::max({inputs, 5u, minimum(V4L2_CID_MIN_BUFFERS_FOR_OUTPUT)});
        outputs = std::max({outputs, 5u, minimum(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)});
    }
    m_queues.inputs = request(kOut, inputs);
    m_queues.outputs = request(kCap, outputs);
    if (!m_queues.inputs || !m_queues.outputs) {
        close();
        return false;
    }
    fprintf(stderr, "venus: буферы %u/%u, extradata %zu/%zu байт\n", m_queues.inputs, m_queues.outputs,
            m_queues.inputExtra, m_queues.packetExtra);
    return true;
}

bool VenusEncoder::start(const std::vector<Buffer> &inputs, const std::vector<Buffer> &outputs)
{
    if (m_fd < 0 || m_streaming || !m_queues.inputs || !m_queues.outputs || inputs.size() != m_queues.inputs
        || outputs.size() != m_queues.outputs
        || !std::all_of(inputs.begin(), inputs.end(), [this](const auto &b) { return fits(b, m_queues.input.size, m_queues.inputExtra); })
        || !std::all_of(outputs.begin(), outputs.end(), [this](const auto &b) { return fits(b, m_queues.packet, m_queues.packetExtra); }))
        return false;
    m_inputs = inputs;
    m_outputs = outputs;
    for (size_t i = 0; i < m_outputs.size(); ++i) {
        if (!queueOutput(int(i))) {
            close();
            return false;
        }
    }
    for (uint32_t type : {kOut, kCap}) {
        uint32_t t = type;
        if (xioctl(m_fd, VIDIOC_STREAMON, &t) < 0) {
            fprintf(stderr, "venus: запуск (%s): %s\n", type == kOut ? "вход" : "выход", strerror(errno));
            close();
            return false;
        }
    }
    m_streaming = true;
    return true;
}

bool VenusEncoder::encode(int input, int64_t timestampUs)
{
    if (!m_streaming || input < 0 || size_t(input) >= m_inputs.size())
        return false;
    v4l2_plane planes[2]{};
    const auto &buffer = m_inputs[input];
    setPlane(planes[0], buffer.fd, buffer.size, m_queues.input.size);
    if (m_queues.inputExtra)
        setPlane(planes[1], buffer.extraFd, m_queues.inputExtra, m_queues.inputExtra);
    v4l2_buffer b{};
    b.type = kOut;
    b.memory = V4L2_MEMORY_USERPTR;
    b.index = uint32_t(input);
    b.m.planes = planes;
    b.length = m_queues.inputExtra ? 2 : 1;
    b.timestamp.tv_sec = timestampUs / 1000000;
    b.timestamp.tv_usec = timestampUs % 1000000;
    if (xioctl(m_fd, VIDIOC_QBUF, &b) < 0) {
        fprintf(stderr, "venus: кадр в кодер: %s\n", strerror(errno));
        return false;
    }
    return true;
}

bool VenusEncoder::setOperatingRate(int rate)
{
    return m_fd >= 0 && setVendorControl(kCidOperatingRate, rate == 0 ? INT32_MAX : std::clamp(rate, 1, 32767) * 65536,
                                         "скорость работы");
}

bool VenusEncoder::requestKeyFrame()
{
    return setControl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "опорный кадр");
}

bool VenusEncoder::setBitrate(int bitsPerSecond)
{
    return setControl(V4L2_CID_MPEG_VIDEO_BITRATE, bitsPerSecond, "битрейт");
}

bool VenusEncoder::takePacket(Packet &out)
{
    v4l2_plane planes[2]{};
    v4l2_buffer b{};
    b.type = kCap;
    b.memory = V4L2_MEMORY_USERPTR;
    b.m.planes = planes;
    b.length = m_queues.packetExtra ? 2 : 1;
    if (xioctl(m_fd, VIDIOC_DQBUF, &b) < 0)
        return false;   // EAGAIN — ещё нет
    if (b.index >= m_outputs.size())
        return false;
    const Buffer &buf = m_outputs[b.index];
    const uint32_t offset = planes[0].data_offset, used = planes[0].bytesused;
    out.timestampUs = int64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
    out.key = b.flags & V4L2_BUF_FLAG_KEYFRAME;
    if (buf.map && used && offset <= buf.size && used <= buf.size - offset) {
        cpuRead(buf.fd, true);
        const auto *p = static_cast<const uint8_t *>(buf.map) + offset;
        out.data.assign(p, p + used);
        cpuRead(buf.fd, false);
    } else {
        out.data.clear();
    }
    queueOutput(int(b.index));
    return true;
}

int VenusEncoder::takeFreeInput()
{
    v4l2_plane planes[2]{};
    v4l2_buffer b{};
    b.type = kOut;
    b.memory = V4L2_MEMORY_USERPTR;
    b.m.planes = planes;
    b.length = m_queues.inputExtra ? 2 : 1;
    if (xioctl(m_fd, VIDIOC_DQBUF, &b) < 0)
        return -1;
    return b.index < m_inputs.size() ? int(b.index) : -1;
}

void VenusEncoder::close()
{
    if (m_fd < 0)
        return;
    if (m_streaming) {
        for (uint32_t type : {kOut, kCap}) {
            uint32_t t = type;
            xioctl(m_fd, VIDIOC_STREAMOFF, &t);
        }
        m_streaming = false;
    }
    ::close(m_fd);
    m_fd = -1;
    m_queues = {};
    m_inputs.clear();
    m_outputs.clear();
}

VenusEncoder::Layout VenusEncoder::nv12Layout(int w, int h)
{
    Layout l;
    l.stride = alignUp(w, 512);
    l.yScan = alignUp(h, 512);
    l.uvScan = alignUp((h + 1) / 2, 256);
    l.size = size_t(l.stride) * (l.yScan + l.uvScan);
    return l;
}

bool VenusEncoder::hasKeyNal(const std::vector<uint8_t> &d, bool hevc)
{
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t h = d[i + 3];
            const int type = hevc ? (h >> 1) & 0x3f : h & 0x1f;
            if (hevc ? (type >= 16 && type <= 21) || type == 32 || type == 33 || type == 34
                     : type == 5 || type == 7 || type == 8)
                return true;
            i += 2;
        }
    }
    return false;
}
