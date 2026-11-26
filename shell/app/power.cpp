#include "power.h"
#include "device_profile.h"

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QWindow>

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

Q_LOGGING_CATEGORY(lcPower, "naomi.power")

namespace {
// Контроллер касаний: после выключения экрана он сбрасывает настройки — фильтр движения снова
// включён (docs/SHELL.md, «Касания»). Права на запись даёт shell-prepare (группа android_input).
// В малом питании панели ядро усыпляет контроллер и отдаёт его сопроцессору датчиков (SLPI) — касания
// до нас не доходят; force_touch_active,1 держит его у процессора: и там касание экрана будит.
const char kTouchCmd[] = "/sys/class/sec/tsp/cmd";
// Датчик отпечатка (fpc1020): на каждое его прерывание — касание пальцем — ядро будит читающих этот файл
// (sysfs_notify, poll с POLLPRI). Служба отпечатков Android держит датчик в режиме навигации: прерывания
// идут и без запроса отпечатка. Права на чтение даёт shell-prepare (группа android_input).
const char kFingerIrq[] = "/sys/devices/platform/soc/soc:fp_fpc1020/irq";
const int kXkbPowerOff = 124;   // KEY_POWER (116) + 8 — на случай, если Qt не узнает клавишу
const int kXkbVolumeDown = 122; // KEY_VOLUMEDOWN (114) + 8
const int kXkbVolumeUp = 123;   // KEY_VOLUMEUP (115) + 8

// Автояркость. Кривая — по восприятию (0…1), от логарифма освещённости: 0 лк → 0,10, 10 → 0,35, 100 → 0,58,
// 1000 → 0,82, 10 000 → 1. Сдвиг рукой — степень, как в Android (BrightnessMappingStrategy):
// v^(3^−сдвиг) — края 0 и 1 остаются на месте, середина ярче или темнее.
qreal luxCurve(int lux)
{
    return qBound(0.0, 0.1 + 0.24 * std::log10(1.0 + qMax(0, lux)), 1.0);
}
qreal adjusted(qreal v, qreal adjust)
{
    return std::pow(qBound(0.001, v, 1.0), std::pow(3.0, -adjust));
}
qreal adjustFor(qreal curve, qreal wanted)   // какой сдвиг даёт wanted при этой кривой
{
    curve = qBound(0.001, curve, 0.999);
    wanted = qBound(0.001, wanted, 0.999);
    return qBound(-1.0, -std::log(std::log(wanted) / std::log(curve)) / std::log(3.0), 1.0);
}
const int kBrightenMs = 1500;     // стало светлее — ярче через 1,5 с, темнее — через 4 с (тень руки не мигает)
const int kDarkenMs = 4000;
const qreal kBrightenRate = 0.6;  // долей шкалы в секунду
const qreal kDarkenRate = 0.25;
const qreal kWakeRate = 3.0;      // первое значение после включения экрана — почти сразу

// AOD (docs/SHELL.md, «AOD») — панель остаётся в обычном режиме, AOD — свой экран при низкой яркости: режим
// малого питания панели (doze) пробовали — при смене режима она гаснет на 50–100 мс, и часы мигали
// (docs/DISPLAY.md, «Малое питание»); оно — только вторая ступень (aodDeep), когда AOD стоит 20 минут без
// движения и мигание некому видеть. Переход — один ход aodAmount на каждом кадре: 0 — экран, 1 — AOD;
// по нему QML переливает часы блокировки в часы AOD (на 4 % меньше), здесь — яркость от прежней к яркости
// AOD: kAodLevel, а если экран и так темнее — та же. Кривая Material «fast out, slow in» (0.4, 0, 0.2, 1):
// трогается и садится мягко.
const int kAodEnterMs = 800;
const int kAodExitMs = 550;
const qreal kAodLevel = 0.3;   // по восприятию: подсветка ≈ 96 из 1023, ≈ 47 нит (2…500 нит линейно, displayconfig redfin)

} // namespace

// Запись яркости — в своём потоке: ядро отдаёт её панели командой и ждёт её — 5,3 мс в среднем, до 11 мс
// (замер, 369 записей). Поток интерфейса на это время стоял, и в переходе AOD (запись на каждом кадре) терялся
// кадр. Пишется последнее значение: не успел — промежуточные не нужны.
class BacklightWriter
{
public:
    explicit BacklightWriter(const QString &path) : m_path(path), m_thread([this] { run(); }) {}
    ~BacklightWriter()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_one();
        m_thread.join();
    }
    void set(int value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_value = value;
        }
        m_wake.notify_one();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [this] { return m_quit || m_value >= 0; });
            if (m_value < 0)
                return;   // выход, всё записано
            const QByteArray value = QByteArray::number(std::exchange(m_value, -1));
            lock.unlock();
            QFile f(m_path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Unbuffered) || f.write(value) != value.size())
                qCWarning(lcPower, "нет доступа к яркости %s", qPrintable(m_path));
            lock.lock();
        }
    }

    const QString m_path;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    int m_value = -1;
    bool m_quit = false;
    std::thread m_thread;
};

namespace {
int volumeDirection(const QKeyEvent *key)
{
    if (key->key() == Qt::Key_VolumeUp || key->nativeScanCode() == kXkbVolumeUp)
        return 1;
    if (key->key() == Qt::Key_VolumeDown || key->nativeScanCode() == kXkbVolumeDown)
        return -1;
    return 0;
}
}

Power::Power()
    : m_setMode(reinterpret_cast<NaomiSetDisplayModeFn>(
          QGuiApplication::platformFunction(kNaomiSetDisplayMode)))
    , m_frameSnapshot(reinterpret_cast<NaomiFrameSnapshotFn>(
          QGuiApplication::platformFunction(kNaomiFrameSnapshot)))
    , m_aodSupported(DeviceProfile::current().pixel)
    , m_noIdleSleep(qEnvironmentVariableIntValue("NAOMI_NO_SLEEP") != 0)
    , m_settings("naomi", "shell")
{
    const auto &profile = DeviceProfile::current();
    const QString backlight = profile.backlight();
    m_maxBacklight = readDeviceFile(backlight + "max_brightness").toInt();
    if (m_maxBacklight <= 0 && profile.pixel)
        m_maxBacklight = 1023;
    if (m_maxBacklight > 0)
        m_backlight = std::make_unique<BacklightWriter>(backlight + "brightness");
    else
        qCWarning(lcPower, "не прочитать max_brightness в %s", qPrintable(backlight));
    m_manual = m_brightness = m_settings.value("display/brightness", 0.6).toReal();
    m_auto = m_settings.value("display/auto", !profile.tablet).toBool();
    m_adjustKnown = m_settings.contains("display/autoAdjust");
    m_adjust = m_settings.value("display/autoAdjust", 0.0).toReal();
    m_aod = aodSupported() && m_settings.value("display/aod", false).toBool();
    m_powerKey = m_settings.value("display/powerKey", "lock").toString();
    if (!aodSupported() && m_powerKey == "aod")
        m_powerKey = "lock";
    m_aodDeep = aodSupported() && m_settings.value("display/aodDeep", true).toBool();
    const QByteArray env = qgetenv("NAOMI_SCREEN_TIMEOUT");
    m_timeoutSec = env.isEmpty() ? m_settings.value("display/timeout", profile.tablet ? 300 : 60).toInt() : env.toInt();
    if (const QByteArray deep = qgetenv("NAOMI_AOD_DEEP"); !deep.isEmpty())
        m_deepSec = qMax(0, deep.toInt());
    m_deepTimer.setSingleShot(true);
    connect(&m_deepTimer, &QTimer::timeout, this, &Power::enterDeep);

    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, &Power::sleep);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        m_ambient = m_lux;
        qCDebug(lcPower, "автояркость: %d лк → %.2f", m_ambient, autoValue());
        rampTo(autoValue(), m_pendingUp ? kBrightenRate : kDarkenRate);
    });
    m_ramp.setInterval(33);
    connect(&m_ramp, &QTimer::timeout, this, &Power::rampStep);
    writeBacklight();
    m_aodCurve.addCubicBezierSegment(QPointF(0.4, 0), QPointF(0.2, 1), QPointF(1, 1));

    if (!profile.pixel)
        return;
    m_fingerFd = ::open(kFingerIrq, O_RDONLY | O_CLOEXEC);
    if (m_fingerFd < 0) {
        qCWarning(lcPower, "нет доступа к датчику отпечатка %s — из AOD будят только экран и кнопка", kFingerIrq);
    } else {
        m_finger = new QSocketNotifier(m_fingerFd, QSocketNotifier::Exception, this);
        m_finger->setEnabled(false);
        connect(m_finger, &QSocketNotifier::activated, this, [this] {
            readFingerIrq();
            if (m_screen == Screen::Aod) {
                qCInfo(lcPower, "касание датчика отпечатка");
                wake();
            }
        });
    }
}

Power::~Power()
{
    if (m_fingerFd >= 0)
        ::close(m_fingerFd);
}

// Прочесть — значит «видел»: следующее прерывание снова разбудит poll
void Power::readFingerIrq()
{
    char buf[8];
    if (::lseek(m_fingerFd, 0, SEEK_SET) < 0 || ::read(m_fingerFd, buf, sizeof buf) < 0)
        qCWarning(lcPower, "датчик отпечатка: чтение %s не удалось", kFingerIrq);
}

void Power::attach(QWindow *window)
{
    window->installEventFilter(this);
    m_window = qobject_cast<QQuickWindow *>(window);
    if (m_window)
        connect(m_window, &QQuickWindow::afterAnimating, this, &Power::aodFrame);
    userActivity();
}

// Погасить: в AOD, если он включён, — переходом
void Power::sleep()
{
    sleepTo(m_aod);
}

void Power::sleepTo(bool aod)
{
    aod = aod && aodSupported();
    if (!aod || m_screen == Screen::Off) {
        setScreen(aod ? Screen::Aod : Screen::Off);
        return;
    }
    if (m_toAod)
        return;
    m_toAod = true;
    if (!m_aodRunning)   // посреди выхода — прежняя яркость та же
        m_awake = m_brightness;
    m_idle.stop();
    m_debounce.stop();
    m_ramp.stop();
    animateAod(1, kAodEnterMs);
    emit dozingChanged();
}

void Power::wake()
{
    if (m_screen == Screen::Off) {
        setScreen(Screen::On);
        return;
    }
    if (!m_toAod)   // включён или уже выходит из AOD
        return;
    m_toAod = false;   // по пути в AOD — назад с того же места
    setScreen(Screen::On);
    animateAod(0, kAodExitMs);
    userActivity();
    emit dozingChanged();
}

// Ход к target (0 или 1); fullMs — на весь путь, идёт по оставшемуся. Считается по кадрам окна
// (aodFrame), а не часами анимаций Qt: те стоят, пока ничего не движется, и на старте принимали простой за
// прошедшее время — ход перескакивал в конец за один кадр
void Power::animateAod(qreal target, int fullMs)
{
    m_aodFrom = m_aodAmount;
    m_aodTo = target;
    m_aodMs = std::max(1, int(fullMs * qAbs(target - m_aodAmount)));
    m_aodRunning = true;
    m_aodClock.invalidate();
    m_aodWarmup = 0;
    if (m_window)
        m_window->update();
}

// Кадр окна (до синхронизации — что поменяли здесь, войдёт в этот кадр). Отсчёт — со второго кадра хода:
// первый начинается, пока поток интерфейса ещё занят тем, что разбудило. Каждый кадр — ровно на кадр панели
// вперёд, как анимации Qt по vsync: момент подготовки кадра гуляет на миллисекунды (после пробуждения поток
// интерфейса разбирает данные naomid), и шаги по часам выходили неровными — от 0,040 до 0,067 хода за кадр.
// По часам — только догнать, если отстал больше чем на 3 кадра
void Power::aodFrame()
{
    if (!m_aodRunning)
        return;
    NaomiFrameSnapshot frames{};
    if (m_frameSnapshot)
        m_frameSnapshot(&frames);
    const qreal period = 1000.0 / (frames.refreshHz > 1 ? frames.refreshHz : 60);
    if (!m_aodClock.isValid()) {
        if (m_aodWarmup++ == 0) {
            m_window->update();
            return;
        }
        m_aodClock.start();
        m_aodT = 0;
    } else {
        m_aodT = std::max(m_aodT + period, m_aodClock.elapsed() - 3 * period);
    }
    const qreal p = qMin(1.0, m_aodT / m_aodMs);
    setAodAmount(m_aodFrom + (m_aodTo - m_aodFrom) * m_aodCurve.valueForProgress(p));
    if (p < 1) {
        m_window->update();
        return;
    }
    m_aodRunning = false;
    if (m_toAod)
        setScreen(Screen::Aod);   // дошли: дальше — AOD (касание и отпечаток будят, данные стоят)
}

// Кадр перехода: яркость — от прежней к яркости AOD (по восприятию)
void Power::setAodAmount(qreal amount)
{
    if (qFuzzyCompare(amount + 1, m_aodAmount + 1))
        return;
    m_aodAmount = amount;
    show(m_awake + (std::min(m_awake, kAodLevel) - m_awake) * amount);
    emit aodAmountChanged();
}

void Power::setScreen(Screen screen)
{
    if (screen == Screen::Aod && !aodSupported())
        screen = Screen::Off;
    if (screen == m_screen)
        return;
    const auto &profile = DeviceProfile::current();
    const bool tablet = profile.tablet;
    const bool lit = screen != Screen::Off;   // AOD — та же включённая панель (малое питание — enterDeep)
    const bool hwOff = profile.displayPowerOff && qgetenv("NAOMI_DISPLAY_POWER_OFF") != "0";
    // Гасим подсветку до HWC2_POWER_MODE_OFF, чтобы на LCD не было вспышки при снятии питания DSI.
    if (tablet && !lit && m_backlight)
        m_backlight->set(0);
    if ((hwOff || !tablet) && (m_deep || (m_screen != Screen::Off) != lit) && m_setMode) {
        if (!m_setMode(lit ? NaomiDisplayOn : NaomiDisplayOff)) {
            if (!tablet)
                return;
            qCWarning(lcPower, "HWC2 power mode (%s) не принят — остаётся управление подсветкой", lit ? "ON" : "OFF");
        }
    } else if (tablet && !lit && !m_backlight) {
        return;
    }
    const bool wasDeep = std::exchange(m_deep, false);
    if (wasDeep) {   // контроллер касаний больше держать незачем
        if (lit)
            QTimer::singleShot(300, this, &Power::applyTouchSettings);
        else
            touchCommand("force_touch_active,0");
    }
    const Screen was = m_screen;
    const bool wasDozing = dozing();
    m_screen = screen;
    if (tablet && !lit)
        m_backlight->set(0);
    qCInfo(lcPower, "экран %s", screen == Screen::On ? "включён" : screen == Screen::Aod ? "в AOD" : "выключен");
    if (was == Screen::Off) {
        if (screen == Screen::Aod) {   // в AOD не переходом (из выключенного) — сразу
            m_toAod = true;
            m_awake = m_brightness;
            m_aodRunning = false;
            setAodAmount(1);
        }
        writeBacklight();
        QTimer::singleShot(300, this, &Power::applyTouchSettings);  // контроллер просыпается не сразу
        if (m_window)   // composer включает панель со следующим выведенным кадром
            m_window->update();
    }
    if (screen == Screen::On) {
        userActivity();
    } else if (screen == Screen::Off && m_aodAmount > 0) {   // из AOD — совсем: включат с прежней яркостью
        m_aodRunning = false;
        m_toAod = false;
        setAodAmount(0);
    }
    if (screen == Screen::Aod && m_aodDeep)
        m_deepTimer.start(m_deepSec * 1000);
    else
        m_deepTimer.stop();
    if (m_finger) {   // прерывания с прошлого AOD (палец водил по датчику при включённом экране) — не в счёт
        if (screen == Screen::Aod)
            readFingerIrq();
        m_finger->setEnabled(screen == Screen::Aod);
    }
    if (screen != Screen::On) {
        m_idle.stop();
        m_debounce.stop();   // при включении освещённость примется заново, сразу
        m_ramp.stop();
        m_ambient = -1;
    }
    if (wasDeep)   // проснулись из малого питания: QML возвращает обои и панель «Background» (AodScreen wallIn)
        emit deepChanged();
    if (was == Screen::On || screen == Screen::On)
        emit displayOnChanged();
    if (dozing() != wasDozing)
        emit dozingChanged();
    if (m_aodDeep && (was == Screen::Aod) != (screen == Screen::Aod))
        emit watchMotionChanged();
}

// Вторая ступень AOD: картинка та же, панель — в малом питании (≈ 30 Гц, яркость двумя ступенями; при смене
// режима гаснет на ≈ 100 мс — после стольких секунд AOD этого никто не видит)
void Power::enterDeep()
{
    if (m_screen != Screen::Aod || !m_aodDeep || m_deep || !m_setMode)
        return;
    touchCommand("force_touch_active,1");   // до малого питания: иначе контроллер успеет уйти к SLPI
    if (!m_setMode(NaomiDisplayDoze)) {
        touchCommand("force_touch_active,0");
        return;
    }
    m_deep = true;
    qCInfo(lcPower, "AOD: панель в малом питании");
    emit deepChanged();   // (картинка малого питания — в том же кадре, с которым панель туда уходит)
    if (m_window)   // composer меняет режим со следующим выведенным кадром
        m_window->update();
}

void Power::leaveDeep()
{
    if (!m_deep || !m_setMode(NaomiDisplayOn))
        return;
    m_deep = false;
    qCInfo(lcPower, "AOD: панель в обычном режиме");
    emit deepChanged();
    QTimer::singleShot(300, this, &Power::applyTouchSettings);
    if (m_window)
        m_window->update();
}

void Power::moved()
{
    if (!watchMotion())
        return;
    m_deepTimer.start(m_deepSec * 1000);
    if (m_deep) {
        qCInfo(lcPower, "AOD: сдвинули");
        leaveDeep();
    }
}

void Power::setAodDeep(bool on)
{
    if (!aodSupported() || on == m_aodDeep)
        return;
    m_aodDeep = on;
    m_settings.setValue("display/aodDeep", on);
    if (m_screen == Screen::Aod) {
        if (on) {
            m_deepTimer.start(m_deepSec * 1000);
        } else {
            m_deepTimer.stop();
            leaveDeep();
        }
        emit watchMotionChanged();
    }
    emit aodDeepChanged();
}

void Power::setAod(bool on)
{
    if (!aodSupported() || on == m_aod)
        return;
    m_aod = on;
    m_settings.setValue("display/aod", on);
    if (!on && m_toAod)   // в AOD или по пути — погасить совсем
        setScreen(Screen::Off);
    emit aodChanged();
}

void Power::setBrightness(qreal value)
{
    value = qBound(0.0, value, 1.0);
    m_ramp.stop();
    if (m_auto && m_ambient >= 0) {
        m_adjust = adjustFor(luxCurve(m_ambient), value);
        m_adjustKnown = true;
        m_settings.setValue("display/autoAdjust", m_adjust);
    } else if (!m_auto) {
        m_manual = value;
        m_settings.setValue("display/brightness", value);
    }
    show(value);
}

void Power::show(qreal value)
{
    if (qFuzzyCompare(value, m_brightness))
        return;
    m_brightness = value;
    if (m_screen != Screen::Off)
        writeBacklight();
    emit brightnessChanged();
}

void Power::setAutoBrightness(bool on)
{
    if (on == m_auto)
        return;
    m_auto = on;
    m_settings.setValue("display/auto", on);
    m_debounce.stop();
    if (on) {
        m_ambient = -1;
        setAmbientLux(m_lux);       // принять сразу, если датчик уже говорит
    } else {
        m_ramp.stop();
        m_manual = m_brightness;    // выключили — остаётся то, что сейчас на экране
        m_settings.setValue("display/brightness", m_manual);
    }
    emit autoBrightnessChanged();
}

qreal Power::autoValue() const
{
    return qBound(0.0, adjusted(luxCurve(m_ambient), m_adjust), 1.0);
}

void Power::setAmbientLux(int lux)
{
    if (lux != m_lux) {
        m_lux = lux;
        emit ambientLuxChanged();
    }
    if (!m_auto || lux < 0 || !awake())
        return;
    if (m_ambient < 0) {             // первое после включения экрана или автояркости
        m_ambient = lux;
        if (!m_adjustKnown) {        // в первый раз — сохранить яркость, выбранную рукой
            m_adjust = adjustFor(luxCurve(lux), m_manual);
            m_adjustKnown = true;
            m_settings.setValue("display/autoAdjust", m_adjust);
        }
        qCDebug(lcPower, "автояркость: %d лк → %.2f (сразу)", lux, autoValue());
        if (m_aodRunning)
            m_awake = autoValue();   // идёт выход из AOD — яркость придёт к новой тем же ходом
        else
            rampTo(autoValue(), kWakeRate);
        return;
    }
    const bool up = lux > m_ambient * 1.1 + 1;
    const bool down = lux < m_ambient * 0.8 - 1;
    if (!up && !down) {              // вернулась в полосу — ничего не менять
        m_debounce.stop();
        return;
    }
    if (!m_debounce.isActive() || m_pendingUp != up) {
        m_pendingUp = up;
        m_debounce.start(up ? kBrightenMs : kDarkenMs);
    }
}

void Power::rampTo(qreal target, qreal perSecond)
{
    m_rampTarget = target;
    m_rampRate = perSecond;
    m_rampClock.start();
    if (!m_ramp.isActive())
        m_ramp.start();
}

void Power::rampStep()
{
    const qreal step = m_rampRate * m_rampClock.restart() / 1000.0;
    const qreal d = m_rampTarget - m_brightness;
    if (qAbs(d) <= step) {
        m_ramp.stop();
        show(m_rampTarget);
    } else {
        show(m_brightness + (d > 0 ? step : -step));
    }
}

void Power::setPowerKey(const QString &action)
{
    static const QStringList known{"lock", "aod", "off", "none"};
    const QString a = known.contains(action) && (aodSupported() || action != "aod") ? action : QStringLiteral("lock");
    if (a == m_powerKey)
        return;
    m_powerKey = a;
    m_settings.setValue("display/powerKey", a);
    emit powerKeyChanged();
}

void Power::setScreenTimeout(int seconds)
{
    if (seconds == m_timeoutSec)
        return;
    m_timeoutSec = qMax(0, seconds);
    m_settings.setValue("display/timeout", m_timeoutSec);
    userActivity();
    emit screenTimeoutChanged();
}

void Power::userActivity()
{
    if (!m_noIdleSleep && m_screen == Screen::On && m_timeoutSec > 0)
        m_idle.start(m_timeoutSec * 1000);
    else
        m_idle.stop();
}

// Яркость по восприятию: квадрат даёт ровный шаг ползунка; минимум 4 — экран не гаснет совсем.
void Power::writeBacklight()
{
    if (!m_backlight)
        return;
    const int minimum = qMin(m_maxBacklight, 4);
    const int value = minimum + int(std::lround((m_maxBacklight - minimum) * m_brightness * m_brightness));
    m_backlight->set(value);
}

void Power::applyTouchSettings()
{
    if (m_deep)
        return;
    touchCommand("force_touch_active,0");
    touchCommand("set_continuous_report_enable,1");
}

void Power::touchCommand(const char *command)
{
    if (!DeviceProfile::current().pixel)
        return;
    QFile f(kTouchCmd);
    if (f.open(QIODevice::WriteOnly))
        f.write(command);
    else
        qCWarning(lcPower, "нет доступа к контроллеру касаний %s", kTouchCmd);
}

bool Power::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_PowerOff || key->nativeScanCode() == kXkbPowerOff) {
            if (!key->isAutoRepeat()) {
                if (!awake())
                    wake();
                else if (m_powerKey == QLatin1String("aod"))
                    sleepTo(true);
                else if (m_powerKey == QLatin1String("off"))
                    sleepTo(false);
                else if (m_powerKey != QLatin1String("none"))
                    sleep();
            }
            return true;
        }
        if (const int dir = volumeDirection(key)) {   // программам не отдаём; погашен или AOD — ничего, пока не смотрят стол
            if (awake())
                userActivity();
            if (awake() || m_desk)
                emit volumeKey(dir);
            return true;
        }
        if (DeviceProfile::current().tablet && !awake()) {
            wake();
            return true;
        }
        userActivity();
        break;
    }
    case QEvent::KeyRelease:
        return volumeDirection(static_cast<QKeyEvent *>(event)) != 0;
    case QEvent::TouchBegin:
        if (dozing() || (DeviceProfile::current().tablet && !awake())) {
            m_wakeTouch = true;
            wake();
            return true;
        }
        m_wakeTouch = false;
        userActivity();
        break;
    case QEvent::TouchUpdate:
        if (m_wakeTouch)
            return true;
        userActivity();
        break;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        if (std::exchange(m_wakeTouch, false))
            return true;
        break;
    case QEvent::MouseButtonPress:
        if (DeviceProfile::current().tablet && !awake()) {
            wake();
            return true;
        }
        userActivity();
        break;
    case QEvent::MouseMove:
    case QEvent::Wheel:
        if (DeviceProfile::current().tablet) {
            if (!awake()) {
                wake();
                return true;
            }
            userActivity();
        }
        break;
    default:
        break;
    }
    return false;
}
