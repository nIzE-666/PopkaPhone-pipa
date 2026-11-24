#include "charge.h"
#include "device_profile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLoggingCategory>
#include <QProcess>
#include <QSettings>

#include <algorithm>

Q_LOGGING_CATEGORY(lcCharge, "naomid.charge")

namespace {

const char CHARGER[] = "/sys/devices/platform/soc/soc:google,charger/";
// кабель: VBUS есть (present). Не online: пока ядро спускает батарею к верху, оно отключает вход, и online — 0
const char USB_PRESENT[] = "/sys/class/power_supply/usb/present";
const char STORE[] = "/userdata/naomi/state/charge.conf";
const char UPOWER[] = "org.freedesktop.UPower";
const char DISPLAY_DEVICE[] = "/org/freedesktop/UPower/devices/DisplayDevice";

const int TICK_MS = 30000;
// жара (docs/NAOMID.md, «Заряд»): 35 °C на кабеле полчаса — верх 60 %, спадает ниже 33 °C; 40 °C — пауза до 37 °C
const double WARM_C = 35, WARM_OFF_C = 33, HOT_C = 40, COOL_C = 37;
const int WARM_S = 1800, WARM_UP = 60;
const int FULL_HOLD_S = 3 * 3600;   // «один раз»: на 100 % на кабеле не дольше 3 ч
// калибровка: раз в 30 дней от 15 % одним заходом до 100 % при батарее холоднее 35 °C (на доке она 33–34 °C в покое),
// полчаса на 100 %
const int CAL_EVERY_S = 30 * 86400, CAL_LOW = 15, CAL_HOLD_S = 1800;
const double CAL_MAX_C = 35;
const int LOW_OFF = 3;
// Xiaomi Pad 6 (pipa): порогов, как у google_charger, в ядре нет. Есть night_charging — «ночная зарядка» MIUI: при
// 1 ядро останавливает заряд от 80 % (насос bq2597x выключается, вход падает до ~7 мА — планшет живёт от батареи),
// при 0 через ~5 с снова PD 8,5 В и 2–3 А (проверено на кабеле 23 ноября 2026). Ниже 80 % так не остановить — верх не
// ниже NIGHT_FLOOR. А вот battery_charging_enabled не трогать: запись в него сбивала PD до 5 В и не возвращала его
const int NIGHT_FLOOR = 80;

struct Limits
{
    int up, lo;
};

QByteArray readSys(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}

qint64 sysNum(const QString &path, qint64 fallback = 0)
{
    bool ok = false;
    const qint64 v = readSys(path).toLongLong(&ok);
    return ok ? v : fallback;
}

bool writeSys(const QString &path, const QByteArray &value)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(value) == value.size();
}

// напряжение покоя батареи на 4,45 В при заряде, мВ: 100 % — замер 21 ноября 2026, остальное — оценка по кривым
// кобальтата с графитом. Для страховки по напряжению: на верхе — не выше этого + 80 мВ
int ocvAt(int pct)
{
    static const int table[][2] = {{0, 3350}, {10, 3700}, {20, 3730}, {30, 3780}, {40, 3820}, {50, 3870},
                                   {60, 3950}, {70, 4030}, {80, 4120}, {90, 4270}, {100, 4410}};
    for (int i = 1; i < 11; ++i)
        if (pct <= table[i][0])
            return table[i - 1][1] + (table[i][1] - table[i - 1][1]) * (pct - table[i - 1][0]) / 10;
    return 4410;
}

double uptimeS()
{
    return readSys("/proc/uptime").split(' ').value(0).toDouble();
}

// серийный номер из памяти батареи — с мусорными байтами: только буквы и цифры
QString cleanSerial(const QByteArray &raw)
{
    QString s;
    for (const char c : raw)
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            s += QLatin1Char(c);
    return s.size() >= 8 ? s : QString();
}

QString findWirelessDevice()
{
    const QDir net(QStringLiteral("/sys/class/net"));
    for (const QString &name : net.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFile::exists(net.filePath(name + QStringLiteral("/wireless"))))
            return name;
    }
    return {};
}

// Энергосбережение Wi-Fi: от батареи — включено (экономит заряд), на кабеле — выключено: с ним точка доступа копит
// пакеты до пробуждения радио, и трансляция стола на Mac и удалённый доступ шли рывками задержки. NetworkManager при
// переподключении ставит своё — проверяем на каждом проходе (iw — раз в 30 с). Имя интерфейса находим сами
// (на Pixel — wlan0, на Xiaomi Pad 6 — wlp1s0).
void applyWifiPowerSave(bool plugged)
{
    const QString dev = findWirelessDevice();
    if (dev.isEmpty())
        return;
    QProcess get;
    get.start("iw", {"dev", dev, "get", "power_save"});
    if (!get.waitForFinished(2000))
        return;
    const bool on = get.readAllStandardOutput().contains(": on");
    const bool want = !plugged;
    if (on == want)
        return;
    if (QProcess::execute("iw", {"dev", dev, "set", "power_save", want ? "on" : "off"}) == 0)
        qCInfo(lcCharge, "Wi-Fi (%s): энергосбережение %s (%s)", qPrintable(dev), want ? "вкл." : "выкл.",
               plugged ? "на кабеле" : "от батареи");
}

} // namespace

Charge::Charge(QObject *parent)
    : BusObject("/org/naomi/Phone/Charge", "org.naomi.Phone.Charge", parent)
{
    const bool pixel = DeviceProfile::current().pixel;
    const bool nightOk = !pixel && QFile::exists(DeviceProfile::battery() + "night_charging");
    set("Backend", pixel ? "google" : nightOk ? "night" : "none");
    set("MinUpper", nightOk ? NIGHT_FLOOR : 50);
    if (!limited()) {
        set("Mode", "off");
        set("Stop", 100);
        return;
    }
    if (night())
        m_nightHold = readSys(DeviceProfile::battery() + "night_charging") == "1";
    QSettings s(QLatin1String(STORE), QSettings::IniFormat);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QString mode = s.value("mode", "daily").toString();
    if (mode != "daily" && mode != "dock" && mode != "custom" && mode != "off")
        mode = "daily";
    set("Mode", mode);
    set("Upper", s.value("upper", 70).toInt());
    set("Lower", s.value("lower", 60).toInt());
    set("Heat", s.value("heat", true).toBool());
    set("Defender", s.value("defender", true).toBool());
    set("Calibrate", s.value("calibrate", true).toBool());
    set("LowOff", s.value("lowoff", true).toBool());
    set("FullOnce", false);
    set("State", "battery");
    set("Warm", false);
    set("Stop", 100);
    set("Start", 0);
    m_lastFull = s.value("lastFull", 0).toLongLong();
    // калибровке отсчёт — с первого запуска службы, а не «давно не было»: иначе первая же загрузка гонит батарею вниз
    m_lastCal = s.value("lastCal", now).toLongLong();
    m_serial = s.value("serial").toString();
    m_serialSince = s.value("serialSince", 0).toLongLong();
    m_cal = Cal(std::clamp(s.value("cal", 0).toInt(), 0, 3));
    m_calNew = s.value("calNew", false).toBool();
    if (m_cal == Cal::Hold)
        m_calSince = now;
    set("LastFull", m_lastFull);
    set("NextCalibration", qlonglong(0));
    if (!s.contains("lastCal"))
        save();

    m_timer.setInterval(TICK_MS);
    connect(&m_timer, &QTimer::timeout, this, &Charge::tick);
    m_timer.start();
    m_soon.setSingleShot(true);
    m_soon.setInterval(1000);   // UPower шлёт пачку изменений — один проход после неё
    connect(&m_soon, &QTimer::timeout, this, &Charge::tick);
    bus().connect(UPOWER, DISPLAY_DEVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                  SLOT(onUPower(QString,QVariantMap,QStringList)));
    applyDefender();
    QMetaObject::invokeMethod(this, &Charge::tick, Qt::QueuedConnection);
}

Charge::~Charge()
{
    // служба уходит — заряд не должен остаться на паузе: на night его, в отличие от порогов google_charger, никто не снимет
    if (night() && m_nightHold)
        writeNight(false);
}

void Charge::onUPower(const QString &, const QVariantMap &changed, const QStringList &)
{
    if (changed.contains("State") || changed.contains("Percentage"))
        m_soon.start();
}

void Charge::save()
{
    QSettings s(QLatin1String(STORE), QSettings::IniFormat);
    s.setValue("mode", mode());
    s.setValue("upper", upper());
    s.setValue("lower", lower());
    s.setValue("heat", heat());
    s.setValue("defender", defender());
    s.setValue("calibrate", calibrate());
    s.setValue("lowoff", lowOff());
    s.setValue("lastFull", m_lastFull);
    s.setValue("lastCal", m_lastCal);
    s.setValue("serial", m_serial);
    s.setValue("serialSince", m_serialSince);
    s.setValue("cal", int(m_cal));
    s.setValue("calNew", m_calNew);
}

void Charge::setCal(Cal phase, bool isNew)
{
    static const char *const names[] = {"нет", "спуск до 15 %", "заряд до 100 %", "полчаса на 100 %"};
    if (phase == m_cal && !isNew)
        return;
    m_cal = phase;
    if (isNew)
        m_calNew = true;
    if (phase == Cal::None)
        m_calNew = false;
    qCInfo(lcCharge, "калибровка%s: %s", m_calNew ? " (новая батарея)" : "", names[int(phase)]);
    save();
}

// ядро (google_charger): 0 ≤ start < stop ≤ 100, 100 / 0 — пороги выключены; неверную пару молча не берёт, отвечая
// успехом. Порядок start = 0 → stop → start проходит из любой прежней пары; потом — перечитать
bool Charge::writeLevels(int stopLevel, int startLevel, int level)
{
    if (night()) {
        // night: коридор ведёт служба. Выше верха — пауза, до низа — пауза держится, ниже низа — заряд
        if (stopLevel < 100)
            stopLevel = std::max(stopLevel, minUpper());
        startLevel = std::clamp(startLevel, 0, stopLevel - 1);
        bool hold = m_nightHold;
        if (stopLevel >= 100 || level <= startLevel)
            hold = false;
        else if (level >= stopLevel)
            hold = true;
        m_stopNow = stopLevel;
        m_startNow = startLevel;
        return writeNight(hold);
    }
    const QString c = CHARGER;
    if (sysNum(c + "charge_stop_level", -1) == stopLevel && sysNum(c + "charge_start_level", -1) == startLevel)
        return true;
    writeSys(c + "charge_start_level", "0");
    writeSys(c + "charge_stop_level", QByteArray::number(stopLevel));
    writeSys(c + "charge_start_level", QByteArray::number(startLevel));
    const int gotStop = sysNum(c + "charge_stop_level", -1), gotStart = sysNum(c + "charge_start_level", -1);
    if (gotStop != stopLevel || gotStart != startLevel) {
        qCWarning(lcCharge, "пороги не встали: хотела %d / %d, в ядре %d / %d", stopLevel, startLevel, gotStop, gotStart);
        return false;
    }
    qCInfo(lcCharge, "пороги: заряжать до %d %%, снова ниже %d %%", stopLevel, startLevel);
    return true;
}

// night: пауза заряда — night_charging = 1, заряд — 0
bool Charge::writeNight(bool hold)
{
    const QString path = DeviceProfile::battery() + "night_charging";
    const QByteArray want = hold ? "1" : "0";
    if (readSys(path) != want && !writeSys(path, want)) {
        qCWarning(lcCharge, "night_charging не записать");
        return false;
    }
    if (hold != m_nightHold) {
        if (hold)
            qCInfo(lcCharge, "заряд на паузе (night_charging): от батареи до %d %%", m_startNow);
        else
            qCInfo(lcCharge, "заряд снова идёт (night_charging выкл.), до %d %%", m_stopNow);
    }
    m_nightHold = hold;
    return true;
}

// TEMP-DEFEND ядра: 6 ч на кабеле при 35 °C и 4,32 В — спустить до 80 %, пока не остынет до 28 °C. С завода — «вхолостую»
// (bd_temp_dry_run = 1, даже в Android). Пока свои пороги стоят, ядро держит его выключенным само — работает он,
// когда предела нет («Без предела», «один раз», калибровка)
void Charge::applyDefender()
{
    if (!google())
        return;   // TEMP-DEFEND — у google_charger; у Xiaomi его нет
    const QString path = QString(CHARGER) + "bd_temp_dry_run";
    const QByteArray want = defender() ? "0" : "1";
    if (readSys(path) != want && !writeSys(path, want))
        qCWarning(lcCharge, "bd_temp_dry_run не записать");
}

void Charge::tick()
{
    const QString b = DeviceProfile::battery();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const int level = int(sysNum(b + "capacity", -1));
    if (level < 0)
        return;   // нет батареи — нечего вести
    const bool plugged = readSys(USB_PRESENT) == "1";
    applyWifiPowerSave(plugged);
    const double tempC = sysNum(b + "temp") / 10.0;
    const int ocvMv = int(sysNum(b + "voltage_ocv") / 1000);
    // + заряд, − разряд. У pipa current_avg нет, а у current_now знак обратный: минус — заряд
    const int avgMa = QFile::exists(b + "current_avg") ? int(sysNum(b + "current_avg") / 1000)
                                                       : int(-sysNum(b + "current_now") / 1000);

    // новая батарея — другой серийный номер, увиденный три прохода подряд (память батареи читается и с мусором):
    // первый заряд до 100 %, чтобы датчик выучил ёмкость
    const QString serial = cleanSerial(readSys(b + "serial_number"));
    if (!serial.isEmpty() && serial != m_serial) {
        m_serialSeen = serial == m_serialCandidate ? m_serialSeen + 1 : 1;
        m_serialCandidate = serial;
        if (m_serial.isEmpty() || m_serialSeen >= 3) {
            const bool replaced = !m_serial.isEmpty();
            m_serial = serial;
            m_serialSince = now;
            if (replaced) {
                qCInfo(lcCharge, "новая батарея");
                m_lastFull = 0;
                m_lastCal = now;
                setCal(Cal::Charge, true);
            }
            save();
        }
    }

    // кабель отключили
    if (m_plugged && !plugged) {
        if (fullOnce() && m_fullSawPlug) {
            set("FullOnce", false);
            m_fullReachedAt = 0;
        }
        if (m_cal == Cal::Hold) {   // дошла до 100 % — калибровка засчитана
            m_lastCal = now;
            setCal(Cal::None);
        } else if (m_cal != Cal::None && !m_calNew) {
            setCal(Cal::None);   // сняли с кабеля — калибровка в другой раз
        }
    }
    m_plugged = plugged;
    if (plugged && fullOnce())
        m_fullSawPlug = true;
    if (level >= 100 && (m_level < 100 || m_lastFull == 0)) {
        m_lastFull = now;
        save();
    }

    // «один раз»: на 100 % на кабеле — не дольше 3 ч
    if (fullOnce() && plugged && level >= 100) {
        if (!m_fullReachedAt)
            m_fullReachedAt = now;
        if (now - m_fullReachedAt >= FULL_HOLD_S) {
            qCInfo(lcCharge, "«один раз»: 3 ч на 100 %%, назад к режиму");
            set("FullOnce", false);
            m_fullReachedAt = 0;
        }
    }

    const QString m = mode();
    const Limits base = m == "dock" ? Limits{55, 45} : m == "custom" ? Limits{upper(), lower()}
                      : m == "off"  ? Limits{100, 0} : Limits{80, 75};
    // night: ниже 80 % ядро заряд не остановит — «док» и низкое «Своё» держатся как 80 / 75 (writeLevels)

    // калибровка: раз в 30 дней, если есть предел. «Каждый день» ждёт, когда сам окажется на кабеле ниже 15 %; на доке
    // (верх до 60 %) — спускается до 15 % сам
    const bool calOn = calibrate() && base.up < 100;
    const qint64 due = std::max(m_lastFull, m_lastCal) + CAL_EVERY_S;
    set("NextCalibration", calOn ? due : qlonglong(0));
    if (m_cal == Cal::None && calOn && now >= due && plugged && !fullOnce()) {
        if (level <= CAL_LOW)
            setCal(Cal::Charge);
        else if (base.up <= 60)
            setCal(Cal::Drain);
    }
    if (m_cal == Cal::Drain && level <= CAL_LOW)
        setCal(Cal::Charge);
    if (m_cal == Cal::Charge && plugged && level >= 100) {
        m_calSince = now;
        setCal(Cal::Hold);
    }
    if (m_cal == Cal::Hold && now - m_calSince >= CAL_HOLD_S) {
        qCInfo(lcCharge, "калибровка: готово");
        m_lastCal = m_lastFull = now;
        setCal(Cal::None);
    }

    // жара
    bool warmNow = false;
    if (heat()) {
        if (tempC >= HOT_C)
            m_hot = true;
        else if (tempC <= COOL_C)
            m_hot = false;
        if (plugged && tempC >= WARM_C) {
            if (!m_warmSince)
                m_warmSince = now;
        } else if (!plugged || tempC < WARM_OFF_C) {
            m_warmSince = 0;
        }
        warmNow = m_warmSince && now - m_warmSince >= WARM_S;
    } else {
        m_hot = false;
        m_warmSince = 0;
    }

    int stopLevel = base.up, startLevel = base.lo;
    if (fullOnce() || m_cal == Cal::Charge || m_cal == Cal::Hold) {
        stopLevel = 100;
        startLevel = 0;
        if (m_cal == Cal::Charge && tempC >= CAL_MAX_C && level < 100) {   // калибровка — только в прохладе: ждать
            stopLevel = std::clamp(level, 1, 99);
            startLevel = 0;
        }
    } else if (m_cal == Cal::Drain) {
        stopLevel = CAL_LOW;
        startLevel = 0;
    } else {
        if (warmNow && stopLevel > WARM_UP) {
            stopLevel = WARM_UP;
            startLevel = std::min(startLevel, WARM_UP - 5);
        }
        // страховка: проценты уплыли, а напряжение покоя выше, чем бывает на верхе, — спускать
        const int guard = base.up < 100 ? ocvAt(base.up) + 80 : 0;
        m_overTicks = guard && ocvMv > guard && level <= stopLevel ? m_overTicks + 1 : 0;
        if (m_overTicks >= 3) {
            if (m_overTicks == 3)
                qCInfo(lcCharge, "напряжение покоя %d мВ выше %d при %d %% — спускаю", ocvMv, guard, level);
            stopLevel = std::max(1, level - 1);
        }
    }
    if (m_hot) {   // пауза: не заряжать, но и не спускать — вход от кабеля остаётся
        stopLevel = std::clamp(std::min(stopLevel, level), 1, 99);
        startLevel = 0;
    }
    if (startLevel >= stopLevel)
        startLevel = std::max(0, stopLevel - 1);
    writeLevels(stopLevel, startLevel, level);
    if (night()) {
        set("Stop", m_stopNow);
        set("Start", m_startNow);
    } else {
        set("Stop", int(sysNum(QString(CHARGER) + "charge_stop_level", 100)));
        set("Start", int(sysNum(QString(CHARGER) + "charge_start_level", 0)));
    }
    set("Warm", warmNow);
    set("LastFull", m_lastFull);

    QString st;
    if (!plugged)
        st = "battery";
    else if (m_hot)
        st = "hot";
    else if (fullOnce())
        st = "full";
    else if (m_cal != Cal::None)
        st = m_calNew ? "learning" : "calibrating";
    else if (night() && m_nightHold)
        st = "resting";   // night: заряд на паузе, планшет — от батареи до низа коридора
    else if (level > stop())
        st = "draining";
    else if (avgMa > 30)
        st = "charging";
    else if (avgMa < -150)   // среднее датчика отстаёт на минуту: после спуска к верху ещё −100 мА
        st = "short";
    else
        st = "holding";
    set("State", st);

    // выключить на 3 % без кабеля — два прохода подряд, не в первые 2 мин после загрузки
    m_lowTicks = lowOff() && !plugged && level <= LOW_OFF ? m_lowTicks + 1 : 0;
    if (m_lowTicks >= 2 && uptimeS() > 120) {
        qCWarning(lcCharge, "заряд %d %% без кабеля — выключаю телефон", level);
        m_lowTicks = -1000;   // один раз
        QProcess::startDetached("systemctl", {"poweroff"});
    }
    m_level = level;
}

void Charge::SetMode(const QString &m)
{
    if (!limited())
        return;
    if (m != "daily" && m != "dock" && m != "custom" && m != "off")
        return;
    if (m == "dock" && minUpper() > 55)
        return;   // night: 55 % ядро не удержит
    set("Mode", m);
    save();
    tick();
}

void Charge::SetLimits(int up, int lo)
{
    if (!limited())
        return;
    up = std::clamp(up, std::max(50, minUpper()), 100);
    lo = std::clamp(lo, 20, up - 5);
    set("Upper", up);
    set("Lower", lo);
    set("Mode", "custom");
    save();
    tick();
}

void Charge::SetOption(const QString &name, bool on)
{
    if (!limited())
        return;
    static const QHash<QString, QString> props = {
        {"heat", "Heat"}, {"defender", "Defender"}, {"calibrate", "Calibrate"}, {"lowoff", "LowOff"}};
    const QString p = props.value(name);
    if (p.isEmpty())
        return;
    set(p, on);
    save();
    if (name == "defender")
        applyDefender();
    tick();
}

void Charge::ChargeFull(bool on)
{
    if (!limited())
        return;
    set("FullOnce", on);
    m_fullSawPlug = on && m_plugged;
    m_fullReachedAt = 0;
    qCInfo(lcCharge, on ? "«до 100 %% один раз»" : "«один раз» — отменено");
    tick();
}

QVariantMap Charge::Health() const
{
    const QString b = DeviceProfile::battery();
    // swelling_data: «temp/soc<TAB>charge(s)<TAB>discharge(s)», дальше строки «30/90<TAB>…<TAB>…» — сколько секунд
    // батарея провела теплее temp °C с зарядом выше soc % на зарядке и без; хранится в памяти самой батареи
    QVariantList swelling;
    const QList<QByteArray> lines = readSys(b + "swelling_data").split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> f = line.simplified().split(' ');
        const QList<QByteArray> ts = f.value(0).split('/');
        bool ok1 = false, ok2 = false;
        const int t = ts.value(0).toInt(&ok1), soc = ts.value(1).toInt(&ok2);
        if (f.size() == 3 && ok1 && ok2)
            swelling.append(QVariantMap{{"tempC", t}, {"soc", soc}, {"chargeS", f[1].toLongLong()},
                                        {"dischargeS", f[2].toLongLong()}});
    }
    return {
        {"fullMah", sysNum(b + "charge_full") / 1000},
        {"designMah", sysNum(b + "charge_full_design") / 1000},
        {"cycles", sysNum(b + "cycle_count")},
        {"resistanceMohm", sysNum(b + "resistance") / 1000},
        {"ocvMv", sysNum(b + "voltage_ocv") / 1000},
        {"tempC", sysNum(b + "temp") / 10.0},
        {"swelling", swelling},
        {"since", m_serialSince},
        {"lastFull", m_lastFull},
        {"nextCalibration", nextCalibration()},
    };
}
