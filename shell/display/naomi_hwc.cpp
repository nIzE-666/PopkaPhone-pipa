#include "naomi_hwc.h"
#include "naomi_display.h"

#include <QtCore/QDateTime>
#include <QtCore/QLoggingCategory>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/qplugin.h>
#include <QtEglFSDeviceIntegration/private/qeglfswindow_p.h>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include <hardware/gralloc.h>
#include <hybris/gralloc/gralloc.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <hybris/hwcomposerwindow/hwcomposer_window.h>

// загрузчик Android-библиотек libhybris (libhybris-common); в hybris/common/binding.h объявлены без
// extern "C", а сами функции — сишные
extern "C" void *android_dlopen(const char *filename, int flag);
extern "C" void *android_dlsym(void *handle, const char *symbol);

Q_LOGGING_CATEGORY(lcNaomiHwc, "naomi.hwc")

NaomiHwcIntegration *NaomiHwcIntegration::s_instance = nullptr;

namespace {

// Сколько кадров ждать после смены режима, прежде чем мерить, и сколько интервалов мерить.
constexpr int kSettleFrames = 12;
constexpr int kKickFrames = 6;
constexpr int kMeasureIntervals = 8;

// V2_4::IComposerClient::VsyncPeriodChangeConstraints и V2_4::VsyncPeriodChangeTimeline
struct VsyncConstraints { int64_t desiredTimeNanos; bool seamlessRequired; };
struct VsyncTimeline { int64_t newVsyncAppliedTimeNanos; bool refreshRequired; int64_t refreshTimeNanos; };
using SetActiveConfigFn = int32_t (*)(void *display, uint32_t config, const VsyncConstraints *,
                                      VsyncTimeline *);
using GetVsyncPeriodFn = int32_t (*)(const void *display, int64_t *period);
using GetAttributeFn = int32_t (*)(void *display, uint32_t config, int32_t attribute);

const char kSetConfigSymbol[] =
    "_ZN7android4HWC24impl7Display30setActiveConfigWithConstraintsEjRKNS_8hardware8graphics8"
    "composer4V2_415IComposerClient28VsyncPeriodChangeConstraintsEPNS6_25VsyncPeriodChangeTimelineE";
const char kVsyncPeriodSymbol[] = "_ZNK7android4HWC24impl7Display21getDisplayVsyncPeriodEPl";
const char kGetAttributeSymbol[] =
    "_ZN7android4HWC24impl7Display12getAttributeEjNS_8hardware8graphics8composer4V2_415IComposerClient9AttributeE";

void *androidDisplay(hwc2_compat_display *display)
{
    return *reinterpret_cast<void **>(display);   // hwc2_compat_display { HWC2::Display *self; }
}

int refreshRates(int *out, int capacity)
{
    auto *self = NaomiHwcIntegration::instance();
    return self ? self->refreshRates(out, capacity) : 0;
}

void frameSnapshot(NaomiFrameSnapshot *out)
{
    if (auto *self = NaomiHwcIntegration::instance())
        *out = self->snapshot();
    else
        *out = {};
}

void displaySizes(NaomiDisplaySizes *out)
{
    if (auto *self = NaomiHwcIntegration::instance())
        *out = self->sizes();
    else
        *out = {};
}

bool requestRefresh(int hz)
{
    auto *self = NaomiHwcIntegration::instance();
    return self && self->requestRefresh(hz);
}

bool setDisplayMode(int mode)
{
    auto *self = NaomiHwcIntegration::instance();
    return self && self->setDisplayMode(mode);
}

const char *modeName(int mode)
{
    return mode == NaomiDisplayOn ? "включён" : mode == NaomiDisplayDoze ? "в малом питании" : "выключен";
}

// Окно Qt на экране: кадр по выводу прошлого (NaomiHwcIntegration::scheduleUpdate).
class NaomiHwcWindow : public QEglFSWindow
{
public:
    using QEglFSWindow::QEglFSWindow;
    void requestUpdate() override
    {
        if (auto *self = NaomiHwcIntegration::instance())
            self->scheduleUpdate(window());
        else
            QEglFSWindow::requestUpdate();
    }
};

class PipaNativeWindow : public HWComposerNativeWindow
{
public:
    PipaNativeWindow(QSize panel, HWCPresentCallback callback, void *data)
        : HWComposerNativeWindow(panel.width(), panel.height(), HAL_PIXEL_FORMAT_RGBA_8888)
        , m_callback(callback), m_data(data)
    {
        setUsage(0);
        setBufferCount(3);
    }

protected:
    int setUsage(uint64_t usage) override
    {
        // QTI gralloc: разрешение чтения CPU оставляет цветовой буфер линейным (без UBWC).
        return HWComposerNativeWindow::setUsage(usage | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER
                                               | GRALLOC_USAGE_SW_READ_RARELY);
    }

    int setBufferCount(int count) override { return HWComposerNativeWindow::setBufferCount(std::max(3, count)); }

    void present(HWComposerNativeWindowBuffer *buffer) override { m_callback(m_data, this, buffer); }

private:
    HWCPresentCallback m_callback;
    void *m_data;
};

} // namespace

struct NaomiHwcIntegration::Listener : HWC2EventListener
{
    NaomiHwcIntegration *self;
    std::atomic<hwc2_display_t> primaryId{0};
};

NaomiHwcIntegration::NaomiHwcIntegration()
{
    s_instance = this;
}

NaomiHwcIntegration::~NaomiHwcIntegration()
{
    s_instance = nullptr;
    delete m_listener;
}

void NaomiHwcIntegration::platformInit()
{
    const auto &profile = DeviceProfile::current();
    setenv("EGL_PLATFORM", "hwcomposer", 0);
    if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_HIDECURSOR"))
        qputenv("QT_QPA_EGLFS_HIDECURSOR", profile.tablet ? "0" : "1");

    m_device = hwc2_compat_device_new(false);
    if (!m_device)
        qFatal("naomi.hwc: нет устройства HWC2 — экраном уже владеет другой композитор?");

    m_listener = new Listener;
    m_listener->on_vsync_received = [](HWC2EventListener *, int32_t, hwc2_display_t, int64_t) {};
    m_listener->on_hotplug_received = [](HWC2EventListener *l, int32_t, hwc2_display_t display,
                                         bool connected, bool primary) {
        auto *self = static_cast<Listener *>(l)->self;
        hwc2_compat_device_on_hotplug(self->m_device, display, connected);
        if (primary && connected)
            static_cast<Listener *>(l)->primaryId = display;
    };
    m_listener->on_refresh_received = [](HWC2EventListener *l, int32_t, hwc2_display_t id) {
        auto *listener = static_cast<Listener *>(l);
        if (DeviceProfile::current().tablet && id == listener->primaryId.load())
            listener->self->requestFullFrame();
    };
    m_listener->self = this;
    hwc2_compat_device_register_callback(m_device, m_listener, 0);

    for (int i = 0; i < 50 && !m_display; ++i) {
        const auto id = profile.tablet ? m_listener->primaryId.load() : 0;
        m_display = hwc2_compat_device_get_display_by_id(m_device, id);
        if (!m_display)
            QThread::msleep(100);
    }
    if (!m_display)
        qFatal("naomi.hwc: основной дисплей не появился");

    hwc2_compat_display_set_power_mode(m_display, HWC2_POWER_MODE_ON);

    void *lib = android_dlopen("libhwc2_compat_layer.so", RTLD_NOW);
    m_setConfigSym = lib ? android_dlsym(lib, kSetConfigSymbol) : nullptr;
    m_vsyncPeriodSym = lib && profile.tablet ? android_dlsym(lib, kVsyncPeriodSymbol) : nullptr;
    if (!m_setConfigSym)
        qCWarning(lcNaomiHwc, "нет setActiveConfigWithConstraints — частота останется как есть");

    readActiveConfig();
    if (profile.tablet && m_panel.width() < m_panel.height()) {
        m_sceneRotation = profile.landscapeRotation;
        bool ok = false;
        const int rotation = qEnvironmentVariableIntValue("NAOMI_ROTATION", &ok);
        if (ok && (rotation == 0 || rotation == 90 || rotation == 270))
            m_sceneRotation = rotation;
    }

    m_modes = profile.displayModes;
    if (profile.tablet) {
        if (m_activeConfigId >= 0)
            m_modes.append({qRound(m_refreshHz.load()), m_activeConfigId});
        const auto attribute = reinterpret_cast<GetAttributeFn>(lib ? android_dlsym(lib, kGetAttributeSymbol) : nullptr);
        if (attribute) {
            // У C API libhybris есть только активный режим. Читаем атрибуты без смены режима.
            // Этот HAL использует малые ID с нуля. Пустые ID между режимами допустимы.
            constexpr uint32_t probeLimit = 32;
            void *display = androidDisplay(m_display);
            for (uint32_t id = 0; id < probeLimit; ++id) {
                if (int(id) == m_activeConfigId)
                    continue;
                const int32_t period = attribute(display, id, HWC2_ATTRIBUTE_VSYNC_PERIOD);
                if (period <= 0 || attribute(display, id, HWC2_ATTRIBUTE_WIDTH) != m_panel.width()
                    || attribute(display, id, HWC2_ATTRIBUTE_HEIGHT) != m_panel.height())
                    continue;
                const int hz = qRound(1e9 / period);
                if (hz >= 24 && hz <= 240 && configForHz(hz) < 0)
                    m_modes.append({hz, int(id)});
            }
        } else {
            qCWarning(lcNaomiHwc, "нет getAttribute — оставляю активный режим pipa");
        }
        std::sort(m_modes.begin(), m_modes.end(), [](const auto &a, const auto &b) { return a.hz < b.hz; });
        for (const auto &mode : m_modes)
            qCInfo(lcNaomiHwc, "режим HWC2 %d: %d Гц", mode.configId, mode.hz);
    }
    // Частоту включает поток отрисовки, когда кадры уже идут (driveRefresh): режим, заданный до
    // первого кадра, composer запоминает, а панель остаётся на старом. Qt сразу знает целевую.
    const int requestedHz = qEnvironmentVariableIntValue("NAOMI_REFRESH");
    const int initialHz = configForHz(requestedHz) >= 0 ? requestedHz : profile.tablet ? 0 : profile.defaultHz;
    if (requestedHz && requestedHz != initialHz)
        qCWarning(lcNaomiHwc, "частота %d Гц недоступна", requestedHz);
    if (requestRefresh(initialHz))
        m_refreshHz = initialHz;  // оценка интервала для Qt до первых кадров, счётчик использует текущую частоту
    m_defaultHz = qRound(m_refreshHz.load());
    // частота покоя: на планшете — 60 Гц (NAOMI_IDLE_HZ — другая, 0 — не снижать); режим должен быть у панели
    const int idleHz = qEnvironmentVariableIsSet("NAOMI_IDLE_HZ") ? qEnvironmentVariableIntValue("NAOMI_IDLE_HZ")
                     : profile.tablet ? 60 : 0;
    m_idleHz = idleHz > 0 && configForHz(idleHz) >= 0 && m_setConfigSym ? idleHz : 0;
    // панель при старте — на частоте покоя: прошлая оболочка ушла в покое (режим composer помнит). Рабочая тогда —
    // высшая до 120 Гц (144 — только выбором в настройках)
    if (m_idleHz && m_defaultHz <= m_idleHz) {
        for (const auto &mode : m_modes)
            if (mode.hz <= 120 && mode.hz > m_defaultHz)
                m_defaultHz = mode.hz;
        m_requestHz = m_defaultHz;
    }
    if (m_idleHz)
        qCInfo(lcNaomiHwc, "частота в покое: %d Гц (2 с без частых кадров), рабочая — %d Гц", m_idleHz, m_defaultHz);

    // Кадр меньше панели (NAOMI_RENDER — ширина, например 720): Qt рисует 720 × 1560, а на панель его растягивает
    // дисплей (MDP) — слой DEVICE с обрезкой по кадру и местом во всю панель. Видеочипу работы меньше, растяжение
    // бесплатно (docs/DISPLAY.md, «Кадр меньше панели»). Плотность — в той же доле: экран в dp тот же.
    m_size = m_panel;
    if (const int w = qEnvironmentVariableIntValue("NAOMI_RENDER"); !profile.tablet && w >= 360 && w < m_panel.width()) {
        const int h = qRound(double(m_panel.height()) * w / m_panel.width() / 2) * 2;
        m_size = QSize(w, h);
    }
    m_deviceLayer = m_size != m_panel;
    const double density = qgetenv("NAOMI_DENSITY").toDouble();
    m_density = (std::isfinite(density) && density > 0 ? density : profile.density)
                * m_size.width() / m_panel.width();

    m_layer = hwc2_compat_display_create_layer(m_display);
    if (!m_layer)
        qFatal("naomi.hwc: composer не создал слой");
    hwc2_compat_layer_set_composition_type(m_layer, m_deviceLayer ? HWC2_COMPOSITION_DEVICE : HWC2_COMPOSITION_CLIENT);
    hwc2_compat_layer_set_blend_mode(m_layer, HWC2_BLEND_MODE_NONE);
    hwc2_compat_layer_set_source_crop(m_layer, 0, 0, m_size.width(), m_size.height());
    hwc2_compat_layer_set_display_frame(m_layer, 0, 0, m_panel.width(), m_panel.height());
    hwc2_compat_layer_set_visible_region(m_layer, 0, 0, m_panel.width(), m_panel.height());
    qCInfo(lcNaomiHwc, "%s: панель %dx%d, кадр %dx%d, поворот сцены %d°, %.1f мм × %.1f мм, плотность %.4g, цель %.2f Гц",
           profile.name, m_panel.width(), m_panel.height(), m_size.width(), m_size.height(), m_sceneRotation,
           m_physicalMm.width(), m_physicalMm.height(), m_density, m_refreshHz.load());
}

void NaomiHwcIntegration::readActiveConfig()
{
    HWC2DisplayConfig *c = hwc2_compat_display_get_active_config(m_display);
    if (!c || c->width <= 0 || c->height <= 0)
        qFatal("naomi.hwc: нет активного режима дисплея");
    m_panel = QSize(c->width, c->height);
    m_activeConfigId = c->vsyncPeriod > 0 ? int(c->id) : -1;
    const int dpi = DeviceProfile::current().panelDpi;
    const qreal dpiX = c->dpiX > 1 ? c->dpiX : dpi, dpiY = c->dpiY > 1 ? c->dpiY : dpi;
    m_physicalMm = QSizeF(c->width / dpiX * 25.4, c->height / dpiY * 25.4);
    setMeasuredHz(c->vsyncPeriod > 0 ? 1e9 / c->vsyncPeriod : 60);
    free(c);
}

int NaomiHwcIntegration::configForHz(int hz) const
{
    for (const auto &mode : m_modes)
        if (mode.hz == hz)
            return mode.configId;
    return -1;
}

int NaomiHwcIntegration::refreshRates(int *out, int capacity) const
{
    if (out)
        for (int i = 0; i < qMin<int>(capacity, m_modes.size()); ++i)
            out[i] = m_modes[i].hz;
    return int(m_modes.size());
}

int NaomiHwcIntegration::kickHz(int hz) const
{
    const int other = hz == 60 ? 90 : 60;
    if (configForHz(other) >= 0)
        return other;
    for (const auto &mode : m_modes)
        if (mode.hz != hz)
            return mode.hz;
    return 0;
}

void NaomiHwcIntegration::setMeasuredHz(double hz)
{
    m_refreshHz = hz;
    m_timer.setPeriodNs(int64_t(1e9 / hz));
}

bool NaomiHwcIntegration::requestRefresh(int hz)
{
    if (!m_setConfigSym || configForHz(hz) < 0)
        return false;
    m_userHz = hz;   // выбор пользователя (настройки, NAOMI_REFRESH) — к нему возвращается покой
    m_requestHz = hz;
    return true;
}

// Поток отрисовки, в present() до driveRefresh. Покой определяем по промежуткам между кадрами: на столе в покое
// рисуют только часы (кадр в секунду), а касание, прокрутка, видео, игра — кадры чаще 10 в секунду
void NaomiHwcIntegration::driveIdle(int64_t nowNs)
{
    constexpr int64_t kBusyGapNs = 100000000;    // кадры ближе 100 мс — работа
    constexpr int64_t kRestAfterNs = 2000000000; // 2 с без работы — покой
    const int64_t gap = m_lastPresentNs ? nowNs - m_lastPresentNs : INT64_MAX;
    m_lastPresentNs = nowNs;
    constexpr int64_t kGraceNs = 300000000;      // после смены частоты — ещё 300 мс не судить
    if (!m_idleHz)
        return;
    if (m_switch != Switch::Idle || m_requestHz.load()) {
        // идёт смена частоты: её проверочные кадры — не работа. И хвост после неё тоже: последний проверочный кадр
        // уже в очереди, когда замер сошёлся, — без запаса он через 15 мс возвращал 120 Гц (журнал 23 ноября 2026)
        m_graceUntilNs = nowNs + kGraceNs;
        m_busyStreak = 0;
        return;
    }
    if (nowNs < m_graceUntilNs)
        return;
    const int user = m_userHz.load();
    const int fast = user ? user : m_defaultHz;
    if (fast <= m_idleHz)
        return;   // выбрана частота не выше покоя — менять нечего
    const int current = m_targetHz ? m_targetHz : m_defaultHz;
    if (gap < kBusyGapNs) {
        m_lastBusyNs = nowNs;
        if (++m_busyStreak >= 2 && current == m_idleHz) {
            qCDebug(lcNaomiHwc, "покой кончился: %d Гц", fast);
            m_requestHz = fast;
        }
    } else {
        m_busyStreak = 0;
        if (current != m_idleHz && nowNs - m_lastBusyNs > kRestAfterNs) {
            qCDebug(lcNaomiHwc, "покой: %d Гц", m_idleHz);
            m_requestHz = m_idleHz;
        }
    }
}

// Поток отрисовки. Внутренний setActiveConfigWithConstraints (в C API libhybris его нет).
bool NaomiHwcIntegration::applyConfig(int hz)
{
    const int config = configForHz(hz);
    if (!m_setConfigSym || config < 0)
        return false;
    VsyncConstraints constraints{0, false};
    VsyncTimeline timeline{};
    const int32_t err = reinterpret_cast<SetActiveConfigFn>(m_setConfigSym)(
        androidDisplay(m_display), uint32_t(config), &constraints, &timeline);
    if (err != 0)
        qCWarning(lcNaomiHwc, "режим %d Гц: composer отказал (ошибка %d)", hz, err);
    return err == 0;
}

// Поток отрисовки, после каждого present(). Частоту подтверждают реальные интервалы вывода, а не
// ответ composer: он может считать режим включённым, когда панель на старом (бывает, если режим
// задали до первого кадра). Тогда «толчок»: на несколько кадров другой режим и снова нужный.
void NaomiHwcIntegration::driveRefresh()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t nowNs = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    if (m_switch == Switch::Idle && m_recheckAt && nowNs >= m_recheckAt) {   // проверка заново: режим и замер
        m_recheckAt = 0;
        int pending = 0;
        m_requestHz.compare_exchange_strong(pending, m_targetHz);
    }
    if (const int hz = m_requestHz.exchange(0)) {
        if (hz != m_targetHz)   // новая частота (не повтор проверки) — повторов снова три
            m_rechecks = 0;
        m_targetHz = hz;
        m_recheckAt = 0;
        if (applyConfig(hz)) {
            m_switch = Switch::Settling;
            setMeasuredHz(hz);
        } else {
            m_switch = Switch::Idle;
            if (HWC2DisplayConfig *active = hwc2_compat_display_get_active_config(m_display)) {
                if (active->vsyncPeriod > 0)
                    setMeasuredHz(1e9 / active->vsyncPeriod);
                free(active);
            }
        }
        m_framesSinceSwitch = 0;
        return;
    }
    if (m_switch == Switch::Idle)
        return;
    ++m_framesSinceSwitch;
    if (m_switch == Switch::Kicking) {
        if (m_framesSinceSwitch >= kKickFrames) {
            m_switch = applyConfig(m_targetHz) ? Switch::Resettling : Switch::Idle;
            m_framesSinceSwitch = 0;
        }
        return;
    }
    if (m_framesSinceSwitch < kSettleFrames)
        return;

    const double measuredMs = m_timer.recentMedianMs(kMeasureIntervals);
    double panelMs = measuredMs;
    if (m_vsyncPeriodSym) {
        int64_t period = 0;
        if (reinterpret_cast<GetVsyncPeriodFn>(m_vsyncPeriodSym)(androidDisplay(m_display), &period) == 0 && period > 0)
            panelMs = period / 1e6;
    }
    if (panelMs <= 0)
        return;  // метки ещё не накопились (fence приходят с задержкой)
    // Интервалы длиннее 25 мс — не частота панели (её режимы — 90 и 60 Гц, 11,1 и 16,7 мс), а паузы в рисовании:
    // экран ничем не занят или оболочка только стартует. По ним не судить — ждать сплошных кадров (любая анимация);
    // иначе после перезагрузки выходило «не включилась: 33 мс» при панели на 90 Гц
    const bool tablet = DeviceProfile::current().tablet;
    if (panelMs > (tablet ? 36 : 25))
        return;
    const double wantedMs = 1000.0 / m_targetHz;
    if (std::fabs(panelMs - wantedMs) < (tablet ? 0.4 : 1.0)) {
        qCInfo(lcNaomiHwc, "частота %d Гц: период %.2f мс, кадр %.2f мс%s", m_targetHz, panelMs, measuredMs,
               m_switch == Switch::Resettling ? " (после толчка)" : "");
        m_switch = Switch::Idle;
        m_rechecks = 0;
    } else if (m_switch == Switch::Settling) {
        qCInfo(lcNaomiHwc, "частота %d Гц: панель даёт %.2f мс — толчок через %d Гц", m_targetHz,
               panelMs, kickHz(m_targetHz));
        m_switch = applyConfig(kickHz(m_targetHz)) ? Switch::Kicking : Switch::Resettling;
        m_framesSinceSwitch = 0;
    } else {
        // Не включилась — или замер соврал: сразу после старта оболочка сама рисует реже панели (кадр 33 мс при
        // панели на 90 Гц — после перезагрузки телефона, 16 ноября 2026). Ещё до трёх проверок через 2 с
        qCWarning(lcNaomiHwc, "частота %d Гц не включилась: период %.2f мс%s", m_targetHz, panelMs,
                  m_rechecks < 3 ? " — проверю снова через 2 с" : "");
        setMeasuredHz(1000.0 / panelMs);
        m_switch = Switch::Idle;
        if (m_rechecks < 3) {
            ++m_rechecks;
            m_recheckAt = nowNs + 2000000000LL;
        }
    }
}

QDpi NaomiHwcIntegration::logicalDpi() const
{
    // логический пиксель Qt = 1 dp Android: 160 × плотность (Pixel 5 — 2,75, как xxhdpi)
    return {160 * m_density, 160 * m_density};
}

QSurfaceFormat NaomiHwcIntegration::surfaceFormatFor(const QSurfaceFormat &input) const
{
    QSurfaceFormat f = input;
    // Qt 6 в Ubuntu собран под настольный OpenGL, а драйвер Adreno (Android EGL) умеет только
    // OpenGL ES — без этого EGL не найдёт ни одной конфигурации. Qt Quick (RHI) работает и с ES.
    f.setRenderableType(QSurfaceFormat::OpenGLES);
    f.setVersion(3, 0);
    f.setRedBufferSize(8);
    f.setGreenBufferSize(8);
    f.setBlueBufferSize(8);
    f.setAlphaBufferSize(8);
    return f;
}

EGLNativeWindowType NaomiHwcIntegration::createNativeWindow(QPlatformWindow *, const QSize &,
                                                            const QSurfaceFormat &)
{
    if (DeviceProfile::current().tablet)
        m_window = new PipaNativeWindow(m_size, &NaomiHwcIntegration::presentCallback, this);
    else
        m_window = HWCNativeWindowCreate(m_size.width(), m_size.height(), HAL_PIXEL_FORMAT_RGBA_8888,
                                         &NaomiHwcIntegration::presentCallback, this);
    if (m_deviceLayer && !m_blank) {
        std::lock_guard<std::mutex> lock(m_hwcMutex);
        m_blank = blankFrame();
    }
    return (EGLNativeWindowType)m_window;
}

// Чёрный кадр размером с панель — клиентский кадр, когда composer не берёт слой с растяжением (present). Буфер — из
// второго окна libhybris размером с панель: его только берём (dequeue) и не возвращаем, окно живёт до выхода (как и
// главное — HWCNativeWindowDestroy падает). Заливка нулями — через gralloc; не вышло — память ION и так нулевая.
ANativeWindowBuffer *NaomiHwcIntegration::blankFrame()
{
    ANativeWindow *win = HWCNativeWindowCreate(m_panel.width(), m_panel.height(), HAL_PIXEL_FORMAT_RGBA_8888,
                                               [](void *, ANativeWindow *, ANativeWindowBuffer *) {}, nullptr);
    if (!win) {
        qCWarning(lcNaomiHwc, "чёрного кадра нет: окно %dx%d не создалось", m_panel.width(), m_panel.height());
        return nullptr;
    }
    native_window_set_buffer_count(win, 1);
    native_window_set_usage(win, GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_SW_WRITE_RARELY);
    ANativeWindowBuffer *b = nullptr;
    int fence = -1;
    if (win->dequeueBuffer(win, &b, &fence) != 0 || !b) {
        qCWarning(lcNaomiHwc, "чёрного кадра нет: буфер %dx%d не выдан", m_panel.width(), m_panel.height());
        return nullptr;
    }
    if (fence >= 0)
        close(fence);   // новый буфер: его никто не рисует
    void *pixels = nullptr;
    const bool filled = hybris_gralloc_lock(b->handle, GRALLOC_USAGE_SW_WRITE_RARELY, 0, 0, b->width, b->height,
                                            &pixels) == 0 && pixels;
    if (filled) {
        memset(pixels, 0, size_t(b->stride) * size_t(b->height) * 4);
        hybris_gralloc_unlock(b->handle);
    }
    qCInfo(lcNaomiHwc, "чёрный кадр %dx%d на случай отказа composer%s", b->width, b->height,
           filled ? "" : " (не залит: gralloc не дал записать)");
    return b;
}

// HWCNativeWindowDestroy в libhybris делает `delete` через указатель на сишную ANativeWindow, а не на
// настоящий класс окна — адрес не тот, процесс падает при выходе («free(): invalid pointer», SIGABRT,
// systemd считает это падением). Окно одно на всё время работы оболочки: при выходе его освобождает
// система, поэтому не удаляем.
void NaomiHwcIntegration::destroyNativeWindow(EGLNativeWindowType)
{
    std::lock_guard<std::mutex> lock(m_hwcMutex);
    m_window = nullptr;
    m_shown = nullptr;
}

void NaomiHwcIntegration::presentCallback(void *self, ANativeWindow *, ANativeWindowBuffer *buffer)
{
    static_cast<NaomiHwcIntegration *>(self)->present(buffer);
}

// Поток отрисовки, после eglSwapBuffers: кадр отдан, и следующий буфер уже получен — после present() окно
// libhybris ещё ждёт, пока панель отпустит буфер (до vsync), и поток отрисовки до того занят.
void NaomiHwcIntegration::presentBuffer(QPlatformSurface *)
{
    QMetaObject::invokeMethod(&m_gui, [this] { frameSwapped(); }, Qt::QueuedConnection);
}

void NaomiHwcIntegration::requestFullFrame()
{
    QMetaObject::invokeMethod(&m_gui, [] {
        for (QWindow *window : QGuiApplication::topLevelWindows())
            QMetaObject::invokeMethod(window, "update");
    }, Qt::QueuedConnection);
}

QEglFSWindow *NaomiHwcIntegration::createWindow(QWindow *window) const
{
    return new NaomiHwcWindow(window);
}

// Когда начинать кадр (docs/SHELL.md, «Касания» и «Окна программ»). Поток отрисовки Qt Quick занят кадром до
// конца eglSwapBuffers, а тот ждёт vsync (≈ 10 мс из 11). Попросит поток интерфейса новый кадр раньше — он
// встаёт в синхронизацию и стоит до vsync. Стоя, он не читает Wayland: кадр программы и её «рисуй дальше»
// (frame callback) проходили только через кадр — пока палец на экране, программы рисовали 45 кадров в
// секунду. Поэтому кадров в пути не больше одного: новый Qt Quick получает, когда обмен буферов прошлого
// кончился (как vsync у Android и Wayland), а если в пути ничего — сразу. Каждый отданный кадр Qt 6 доводит
// до обмена буферов (кадров «без изменений» не бывает); сторож — на случай, если окно не готово рисовать.
void NaomiHwcIntegration::scheduleUpdate(QWindow *window)
{
    if (!m_waiting.contains(window))
        m_waiting.append(window);
    if (m_inFlight || m_flushPosted)
        return;
    m_flushPosted = true;   // не изнутри requestUpdate — из цикла событий
    QMetaObject::invokeMethod(&m_gui, [this] {
        m_flushPosted = false;
        flushUpdates();
    }, Qt::QueuedConnection);
}

void NaomiHwcIntegration::frameSwapped()
{
    m_inFlight = false;
    if (m_watchdog)
        m_watchdog->stop();
    flushUpdates();
}

void NaomiHwcIntegration::flushUpdates()
{
    if (m_inFlight || m_waiting.isEmpty())
        return;
    if (!m_watchdog) {
        m_watchdog = new QTimer(&m_gui);
        m_watchdog->setSingleShot(true);
        QObject::connect(m_watchdog, &QTimer::timeout, &m_gui, [this] {
            qCWarning(lcNaomiHwc, "кадр не дошёл до вывода за 100 мс — следующий без ожидания");
            frameSwapped();
        });
    }
    const QList<QPointer<QWindow>> waiting = std::exchange(m_waiting, {});
    for (const QPointer<QWindow> &w : waiting) {
        if (!w || !w->handle() || !w->handle()->hasPendingUpdateRequest())
            continue;
        m_inFlight = true;          // до deliver: внутри него Qt Quick уже просит следующий кадр
        m_watchdog->start(100);
        w->handle()->deliverUpdateRequest();
    }
}

// Малое питание: частоту в нём не трогаем (она — панели); после него, как и после выключения, — заново.
bool NaomiHwcIntegration::setDisplayMode(int mode)
{
    if (mode == NaomiDisplayDoze && !DeviceProfile::current().pixel)
        return false;
    std::lock_guard<std::mutex> lock(m_hwcMutex);
    if (mode == m_mode)
        return true;
    const hwc2_power_mode_t hwcMode = mode == NaomiDisplayOn ? HWC2_POWER_MODE_ON
                                    : mode == NaomiDisplayDoze ? HWC2_POWER_MODE_DOZE : HWC2_POWER_MODE_OFF;
    const hwc2_error_t err = hwc2_compat_display_set_power_mode(m_display, hwcMode);
    if (err != HWC2_ERROR_NONE) {
        qCWarning(lcNaomiHwc, "дисплей %s: ошибка %d", modeName(mode), err);
        return false;
    }
    m_mode = mode;
    qCInfo(lcNaomiHwc, "дисплей %s", modeName(mode));
    if (mode == NaomiDisplayOn) {  // панель может вернуться на 60 Гц — заново, с проверкой замером
        // выбранная частота, а не текущая: экран гас в покое — m_targetHz там частота покоя
        const int user = m_userHz.load();
        m_requestHz = user ? user : m_defaultHz ? m_defaultHz : m_targetHz ? m_targetHz : int(m_refreshHz.load());
    }
    return true;
}

// Вызывается из eglSwapBuffers (поток отрисовки Qt Quick): отдать буфер панели.
void NaomiHwcIntegration::present(ANativeWindowBuffer *buffer)
{
    std::lock_guard<std::mutex> lock(m_hwcMutex);
    const int acquire = HWCNativeBufferGetFence(buffer);  // готовность отрисовки (GPU)
    if (m_mode == NaomiDisplayOff) {  // буфер вернётся EGL с меткой завершения GPU
        m_shown = nullptr;
        return;
    }
    const bool tablet = DeviceProfile::current().tablet;
    if (tablet && (!buffer->handle || buffer->width != m_panel.width() || buffer->height != m_panel.height()
                   || buffer->stride < buffer->width || buffer->format != HAL_PIXEL_FORMAT_RGBA_8888))
        qFatal("naomi.hwc: неверный client target pipa: %dx%d, stride %d, format 0x%x",
               buffer->width, buffer->height, buffer->stride, buffer->format);
    if (tablet && buffer == m_shown)
        qFatal("naomi.hwc: pipa повторно выдала буфер, который composer ещё не освободил");
    if (tablet && !m_presented)
        qCInfo(lcNaomiHwc, "client target pipa %dx%d, stride %d, RGBA8888, usage 0x%llx",
               buffer->width, buffer->height, buffer->stride, static_cast<unsigned long long>(buffer->usage));
    // libhybris передаёт владение копией fence в Android. Если кадр отброшен, оригинал остаётся у EGL.
    auto submit = [&](auto setter) {
        const int fd = acquire < 0 ? -1 : fcntl(acquire, F_DUPFD_CLOEXEC, 0);
        if (acquire >= 0 && fd < 0) {
            qCWarning(lcNaomiHwc, "не скопировать acquire fence: %s", strerror(errno));
            return false;
        }
        const auto err = setter(fd);
        if (err != HWC2_ERROR_NONE)
            qCWarning(lcNaomiHwc, "буфер HWC2: ошибка %d", err);
        return err == HWC2_ERROR_NONE;
    };
    // Кадр меньше панели — слой DEVICE: буфер и его готовность — слою (до validate: composer выбирает по нему). Вид
    // слоя — заново каждый кадр: сразу после перезагрузки телефона composer на первый кадр просит «составлю сам»
    // (CLIENT), и accept_changes делал слой клиентским навсегда — экран оставался чёрным
    if (m_deviceLayer) {
        hwc2_compat_layer_set_composition_type(m_layer, HWC2_COMPOSITION_DEVICE);
        if (!submit([&](int fd) { return hwc2_compat_layer_set_buffer(m_layer, 0, buffer, fd); }))
            return;
    }
    uint32_t types = 0, requests = 0;
    hwc2_error_t err = hwc2_compat_display_validate(m_display, &types, &requests);
    if (err != HWC2_ERROR_NONE && err != HWC2_ERROR_HAS_CHANGES) {
        qCWarning(lcNaomiHwc, "validate: ошибка %d", err);
        return;
    }
    // Отказ (хочет составлять сам): после перезагрузки телефона composer так отвечает, пока не покажет хоть один
    // кадр, составленный им самим (замер: кадры без показа — 570 отказов в секунду, экран чёрный). Показываем ему
    // чёрный кадр во всю панель клиентским кадром (blankFrame), а кадр Qt рисуем снова — он уже слоем DEVICE. Свой
    // кадр клиентским отдавать нельзя: composer (SDM Qualcomm) подгоняет размер кадра дисплея под клиентский буфер —
    // после буфера 720 × 1560 смеситель дисплея становился 720 × 1560, место слоя (вся панель) обрезалось по нему, и
    // на экране был левый верхний угол картинки в 1,5 раза крупнее — до конца работы composer (16 ноября 2026, журнал
    // драйвера: смеситель 2 × 360 × 1560, из кадра — 480 × 1040; docs/DISPLAY.md, «Кадр меньше панели»)
    const bool refused = m_deviceLayer && types;
    if (refused) {
        ++m_refusedSec;
        if (!std::exchange(m_refusing, true))
            qCWarning(lcNaomiHwc, "composer не берёт слой с растяжением (хочет составлять сам) — чёрный кадр во всю "
                                  "панель, кадр — снова");
    }
    if (const int64_t now = QDateTime::currentMSecsSinceEpoch(); now - m_refusedFrom >= 1000) {
        if (m_refusedSec)
            qCWarning(lcNaomiHwc, "composer отказал в растяжении: %d кадров за секунду", m_refusedSec);
        m_refusedSec = 0;
        m_refusedFrom = now;
    }
    if (refused && !m_blank)
        return;
    if (err == HWC2_ERROR_HAS_CHANGES || types || requests) {
        err = hwc2_compat_display_accept_changes(m_display);
        if (err != HWC2_ERROR_NONE) {
            qCWarning(lcNaomiHwc, "acceptChanges: ошибка %d", err);
            return;
        }
    }
    if (!m_deviceLayer && !submit([&](int fd) {
            return hwc2_compat_display_set_client_target(m_display, 0, buffer, fd, HAL_DATASPACE_UNKNOWN);
        }))
        return;
    if (refused && hwc2_compat_display_set_client_target(m_display, 0, m_blank, -1, HAL_DATASPACE_UNKNOWN)
                       != HWC2_ERROR_NONE)
        return;
    if (m_deviceLayer && !refused && std::exchange(m_refusing, false))
        qCInfo(lcNaomiHwc, "composer снова берёт слой с растяжением");
    int32_t presentFence = -1;
    err = hwc2_compat_display_present(m_display, &presentFence);
    if (err != HWC2_ERROR_NONE) {
        qCWarning(lcNaomiHwc, "present: ошибка %d", err);
        if (presentFence >= 0)
            close(presentFence);
        return;
    }
    if (refused) {
        // Кадр Qt вернётся с acquire. Прошлый показанный — когда панель покажет чёрный.
        if (m_shown && m_shown != buffer) {
            HWCNativeBufferSetFence(m_shown, presentFence);
            presentFence = -1;
        }
        m_shown = nullptr;
        if (presentFence >= 0)
            close(presentFence);
        requestFullFrame();
        return;
    }

    // Когда буфер свободен. Клиентский слой (client target): getReleaseFences отдаёт метки только слоям, которые
    // составляет сам дисплей, — для него всегда −1 (замер). С этой −1 окно libhybris (два буфера по кругу) отдавало
    // буфер под новый кадр сразу — видеочип рисовал в буфер, который панель ещё показывала: при прокрутке —
    // горизонтальные полосы и белые точки у краёв (docs/DISPLAY.md, «Буферы»). Как в Android (FramebufferSurface):
    // буфер прошлого кадра панель отпускает, когда показала этот, — его метка — present fence этого кадра (копия:
    // сам fence забирает таймер). У этого буфера метка появится со следующим кадром; готовность (acquire) ушла в
    // composer вместе с ним. Слой DEVICE (кадр меньше панели) получает свою метку — она и есть «прошлый буфер
    // отпущен» (по HWC2 метка present относится к буферу, который этот кадр сменил); нет её — та же present fence.
    int32_t release = -1;
    if (m_deviceLayer) {
        hwc2_compat_out_fences_t *fences = nullptr;
        if (hwc2_compat_display_get_release_fences(m_display, &fences) == HWC2_ERROR_NONE && fences) {
            release = hwc2_compat_out_fences_get_fence(fences, m_layer);
            hwc2_compat_out_fences_destroy(fences);
        }
    }
    if (m_shown && m_shown != buffer) {
        if (release < 0 && presentFence >= 0) {
            release = fcntl(presentFence, F_DUPFD_CLOEXEC, 0);
            if (release < 0)
                release = std::exchange(presentFence, -1);   // повторное использование буфера важнее счётчика
        }
        HWCNativeBufferSetFence(m_shown, release);
    } else if (release >= 0) {
        close(release);
    }
    if (acquire >= 0)
        close(acquire);
    HWCNativeBufferSetFence(buffer, -1);
    m_shown = buffer;
    if (presentFence >= 0)
        m_timer.add(presentFence);
    else if (tablet && !m_presented)
        qCWarning(lcNaomiHwc, "первый кадр pipa: HAL вернул present без fence");
    ++m_presented;
    if (m_mode != NaomiDisplayOn)
        return;  // в малом питании частота — панели
    if (m_presented >= 2) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        driveIdle(int64_t(now.tv_sec) * 1000000000 + now.tv_nsec);
        driveRefresh();
        if (tablet && m_switch != Switch::Idle && m_framesSinceSwitch < kSettleFrames + kMeasureIntervals)
            requestFullFrame();
    } else if (m_requestHz.load()) {
        requestFullFrame();
    }
}

QFunctionPointer NaomiHwcIntegration::platformFunction(const QByteArray &function) const
{
    if (function == kNaomiFrameSnapshot)
        return QFunctionPointer(&frameSnapshot);
    if (function == kNaomiDisplaySizes)
        return QFunctionPointer(&displaySizes);
    if (function == kNaomiSetRefresh && m_setConfigSym && !m_modes.isEmpty())
        return QFunctionPointer(&::requestRefresh);
    if (function == kNaomiRefreshRates && m_setConfigSym)
        return QFunctionPointer(&::refreshRates);
    if (function == kNaomiSetDisplayMode)
        return QFunctionPointer(&::setDisplayMode);
    return nullptr;
}

class NaomiHwcPlugin : public QEglFSDeviceIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QEglFSDeviceIntegrationFactoryInterface_iid FILE "eglfs_naomi.json")
public:
    QEglFSDeviceIntegration *create() override { return new NaomiHwcIntegration; }
};

#include "naomi_hwc.moc"
