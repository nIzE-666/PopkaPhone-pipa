// НаомиФон — что драйвер экрана eglfs_naomi отдаёт программам.
// Достаётся через QGuiApplication::platformFunction(имя): заголовок общий у драйвера и программ,
// своих библиотек между ними нет.
#pragma once

#include <cstdint>

// Кадры по реальным меткам вывода на панель (present-fence), не по таймерам Qt.
struct NaomiFrameSnapshot {
    double refreshHz;   // текущий режим панели
    double fps;         // кадров выведено за последнюю секунду
    double medianMs;    // интервалы между выводами за последнюю секунду
    double p95Ms;
    double maxMs;
    uint64_t frames;    // выведено с запуска
    uint64_t drops;     // пропусков с запуска: пауз от 1,5 до 6 периодов (длиннее — покой, не пропуск)
};

using NaomiFrameSnapshotFn = void (*)(NaomiFrameSnapshot *out);
using NaomiSetRefreshFn = bool (*)(int hz);  // режим панели; true — заявка принята
using NaomiRefreshRatesFn = int (*)(int *out, int capacity);   // nullptr — узнать длину
// Режим панели (docs/DISPLAY.md, «Малое питание»): выключена — кадры не выводятся; включена — частота
// восстанавливается; малое питание (doze composer HAL) — кадры выводятся, частота (≈ 30 Гц) и яркость (две
// ступени) — панели. Composer применяет режим со следующим выведенным кадром: после смены нужен кадр.
enum NaomiDisplayMode : int { NaomiDisplayOff = 0, NaomiDisplayOn = 1, NaomiDisplayDoze = 2 };
using NaomiSetDisplayModeFn = bool (*)(int mode);

// Размеры в пикселях: кадр, который рисует Qt, и панель. Кадр меньше панели — его растягивает дисплей
// (NAOMI_RENDER, docs/DISPLAY.md, «Кадр меньше панели»).
struct NaomiDisplaySizes {
    int renderWidth, renderHeight;
    int panelWidth, panelHeight;
    int sceneRotation;
};
using NaomiDisplaySizesFn = void (*)(NaomiDisplaySizes *out);

inline constexpr const char kNaomiFrameSnapshot[] = "naomi_frame_snapshot";
inline constexpr const char kNaomiDisplaySizes[] = "naomi_display_sizes";
inline constexpr const char kNaomiSetRefresh[] = "naomi_set_refresh";
inline constexpr const char kNaomiRefreshRates[] = "naomi_refresh_rates";
inline constexpr const char kNaomiSetDisplayMode[] = "naomi_set_display_mode";
