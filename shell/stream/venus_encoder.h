// НаомиФон — аппаратный видеокодер Qualcomm Venus через V4L2 (/dev/video/venus_enc), без Android.
// Особенности драйвера msm_vidc (docs/STREAM.md, «Видеокодер»): буферы — только USERPTR, и это номер dma-buf
// (planes[].reserved[0]), а не адрес; размеры кадра он знает только после выбора кодека; вход — NV12
// с выравниванием 512. Буферы выделяет вызывающий (gralloc_buffer.h): кодеру — только их fd.
// Поток вызовов: open → (выделить буферы по layout) → start → encode / poll → takePacket, takeFreeInput.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class VenusEncoder
{
public:
    // Наибольшая «скорость работы» для кадра, к/с: драйвер считает нагрузку — блоков 16 × 16 в секунду при этой
    // скорости — и больше 1 958 400 (паспорт кодера SM7250) не запускает («Invalid argument» на запуске); кодер
    // сжимает тем быстрее, чем она выше (2400 × 1348: 75 — 11,2 мс, 120 — 8,6, 150 — 7,9)
    static int maxOperatingRate(int width, int height)
    {
        const int blocks = ((width + 15) / 16) * ((height + 15) / 16);
        return std::clamp(1958400 * 98 / 100 / blocks, 30, 240);
    }
    // То же, но с местом для декодеров. Видеокодек у телефона один, и запас его частоты делят все сеансы: каждый
    // занимает блоков в секунду × «скорость работы». Кодер стола на наибольшей скорости (2400 × 1348 — 150) оставлял
    // место одному ролику 1080p60; 4K30 или второй ролик драйвер уже не пускал («Core cannot support this load» —
    // плеер Android с ошибкой, окно с дырой). На 120 проходит и 4K30 (замер 20 ноября 2026: 150 — отказ, 135 не
    // мерили, 120 и 100 — идёт); цена — сжатие 8,7 → 9,8 мс. Доля — нагрузка кадра 2400 × 1348 на 120
    static int sharedOperatingRate(int width, int height)
    {
        const int blocks = ((width + 15) / 16) * ((height + 15) / 16);
        return std::clamp(1530000 / blocks, 30, maxOperatingRate(width, height));
    }

    struct Settings {
        int width = 720;
        int height = 1560;
        int fps = 90;
        int bitrate = 60'000'000;   // бит/с — цель; с потолком QP кадр, которому надо больше, её превышает
        // Самое грубое квантование (QP), какое кодеру можно: выше — блоки, «квадратики». 22 — без них и на
        // резких сменах всего кадра (docs/STREAM.md, «Качество»); 0 — без потолка (как решит кодер — до 51)
        int maxQp = 22;
        bool hevc = false;
        int operatingRate = -1;     // «скорость работы» (частоты кодера), к/с: −1 — как fps, 0 — наибольшая
        // режим малой задержки кодера (своя настройка Qualcomm). Сжатие быстрее (кадр стола 2400 × 1348 в трансляции:
        // 6,8 мс против 9,0), но кодер вдвое расточительнее — 35 Мбит/с вместо 16 на том же движении, и связь с
        // распаковкой у зрителя съедают выигрыш (docs/STREAM.md, «Кодер: честный замер»). Поэтому выключен
        bool lowLatency = false;
        std::vector<std::pair<uint32_t, int32_t>> extra;   // ещё настройки V4L2 (разведка: naomi-venc-bench --ctrl)
    };
    // раскладка входного кадра NV12, как её требует кодер
    struct Layout {
        int stride = 0;       // байт в строке
        int yScan = 0;        // строк яркости (с выравниванием); с неё — цветность
        int uvScan = 0;
        size_t size = 0;      // байт в кадре
    };
    struct Queues {
        Layout input;
        size_t inputExtra = 0;
        size_t packet = 0;
        size_t packetExtra = 0;
        unsigned inputs = 0, outputs = 0;
    };
    struct Buffer {
        int fd = -1;
        size_t size = 0;
        const void *map = nullptr;   // для выходных: вид для процессора
        int extraFd = -1;
        size_t extraSize = 0;
    };
    struct Packet {
        std::vector<uint8_t> data;   // H.264/HEVC Annex B
        int64_t timestampUs = 0;     // тот, что дан encode()
        bool key = false;            // по флагу кодера
    };

    ~VenusEncoder();
    bool open(const Settings &s);
    bool prepareQueues(unsigned inputs, unsigned outputs);
    const Queues &queues() const { return m_queues; }
    bool start(const std::vector<Buffer> &inputs, const std::vector<Buffer> &outputs);
    bool encode(int input, int64_t timestampUs);   // кадр готов в inputs[input]
    bool requestKeyFrame();
    // «скорость работы» на ходу (драйвер меняет частоты кодека сразу)
    bool setOperatingRate(int rate);
    bool setBitrate(int bitsPerSecond);
    int pollFd() const { return m_fd; }
    bool takePacket(Packet &out);   // сжатый кадр, если готов (не ждёт)
    int takeFreeInput();            // вход, который кодер прочёл; −1 — нет
    void close();

    // начинается ли кадр с опорного (IDR) или параметров (SPS/PPS) — по самим NAL
    static bool hasKeyNal(const std::vector<uint8_t> &annexB, bool hevc);
    // раскладка NV12 кодера для кадра w × h (SM7250: «512» — строка и строки яркости кратны 512, цветности — 256);
    // open() сверяет её с тем, что сказал драйвер
    static Layout nv12Layout(int w, int h);

private:
    bool setControl(uint32_t id, int32_t value, const char *name);
    bool setVendorControl(uint32_t id, int32_t value, const char *name);
    bool queueOutput(int index);

    int m_fd = -1;
    Queues m_queues;
    bool m_kona = false;
    std::vector<Buffer> m_inputs, m_outputs;
    bool m_streaming = false;
};
