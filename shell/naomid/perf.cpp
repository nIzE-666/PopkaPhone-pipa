#include "perf.h"

#include "device_profile.h"
#include "gpu.h"

#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/resource.h>

Q_LOGGING_CATEGORY(lcPerf, "naomid.perf")

namespace {

const char kCpufreq[] = "/sys/devices/system/cpu/cpufreq/";
const char kCpuBoost[] = "/sys/devices/system/cpu/cpu_boost/";
const char kKgsl[] = "/sys/class/kgsl/kgsl-3d0/";
const char kStune[] = "/dev/stune";
const char kShellGroup[] = "/dev/stune/shell";
const char kShellBoost[] = "20";
const int kShellNice = -5;
const char kAppGroup[] = "/dev/stune/top-app";
const char kAppBoost[] = "10";
const char kSkinCpuOffset[] = "/sys/devices/virtual/thermal/tz-by-name/skin-therm-cpu/offset";
const char kSkinCpuShift[] = "-4000";
const char kCpuLatency[] = "/dev/cpu_dma_latency";
const qint32 kCpuLatencyUs = 100;
const char kStore[] = "/userdata/naomi/state/perf.conf";
const char kWakeLockName[] = "naomi-pipa";
const char kScreenState[] = "/sys/class/thermal/thermal_message/screen_state";

bool writeFile(const QString &path, const QByteArray &value, bool quiet = false)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly) && f.write(value) == value.size())
        return true;
    if (!quiet)
        qCWarning(lcPerf, "%s <- %s: %s", qPrintable(path), value.constData(), qPrintable(f.errorString()));
    return false;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}

bool usbPlugged()
{
    return readFile(DeviceProfile::usbSupply() + QStringLiteral("present")) == "1";
}

bool hasActiveSshSession()
{
    // Не уводим планшет в mem-сон, пока открыт интерактивный SSH/терминальный сеанс разработки.
    const QDir proc(QStringLiteral("/proc"));
    for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (pid.isEmpty() || !pid.at(0).isDigit())
            continue;
        const QByteArray comm = readFile(proc.filePath(pid + QStringLiteral("/comm")));
        if (comm != "sshd")
            continue;
        const QByteArray cmd = readFile(proc.filePath(pid + QStringLiteral("/cmdline")));
        if (cmd.contains("@pts/"))
            return true;
    }
    return false;
}

QString normalizeProfile(const QString &raw)
{
    const QString s = raw.trimmed().toLower();
    if (s == QLatin1String("eco") || s == QLatin1String("powersave"))
        return QStringLiteral("eco");
    if (s == QLatin1String("performance") || s == QLatin1String("max"))
        return QStringLiteral("performance");
    return QStringLiteral("balanced");
}

} // namespace

Perf::Perf(Gpu *gpu, QObject *parent)
    : BusObject("/org/naomi/Phone/Perf", "org.naomi.Phone.Perf", parent)
    , m_gpu(gpu)
{
    set("Desk", false);
    set("Profile", QStringLiteral("balanced"));
    set("StayAwake", false);
    set("DeepSuspend", false);
    set("ThermalLevel", 0);
    set("SkinTempC", 0.0);

    const auto &prof = DeviceProfile::current();
    if (prof.pixel) {
        m_watch.setConnection(bus());
        m_watch.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
        connect(&m_watch, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &who) {
            m_watch.removeWatchedService(who);
            if (m_holders.remove(who)) {
                qCInfo(lcPerf, "стол на полную: %s ушёл с шины — отпускаю", qPrintable(who));
                apply();
            }
        });
        floors(false);
        skin(false);
        boostApps(false);
        if (QFile::exists(QLatin1String(kShellGroup)))
            boostShell(false);
        return;
    }

    if (!prof.tablet)
        return;

    discoverTopology();
    QSettings s(QLatin1String(kStore), QSettings::IniFormat);
    set("Profile", normalizeProfile(s.value(QStringLiteral("profile"), QStringLiteral("balanced")).toString()));
    set("StayAwake", s.value(QStringLiteral("stayAwake"), false).toBool());
    set("DeepSuspend", s.value(QStringLiteral("deepSuspend"), false).toBool());

    m_thermalTimer.setInterval(5000);
    connect(&m_thermalTimer, &QTimer::timeout, this, &Perf::pollThermal);

    m_suspendTimer.setSingleShot(true);
    m_suspendTimer.setInterval(15000);
    connect(&m_suspendTimer, &QTimer::timeout, this, &Perf::tryDeepSuspend);

    applyTabletProfile();
    updateWakeLock();
    pollThermal();
    m_thermalTimer.start();
}

Perf::~Perf()
{
    if (m_wakeLockHeld)
        writeFile(QStringLiteral("/sys/power/wake_unlock"), kWakeLockName, true);
}

void Perf::discoverTopology()
{
    m_policies.clear();
    const QDir dir(QString::fromLatin1(kCpufreq));
    static const QRegularExpression re(QStringLiteral("^policy(\\d+)$"));
    for (const QString &entry : dir.entryList({QStringLiteral("policy*")}, QDir::Dirs)) {
        const auto m = re.match(entry);
        if (!m.hasMatch())
            continue;
        PolicyInfo info;
        info.firstCpu = m.captured(1).toInt();
        info.dir = dir.filePath(entry);
        info.minKHz = readFile(info.dir + QStringLiteral("/cpuinfo_min_freq")).toInt();
        info.maxKHz = readFile(info.dir + QStringLiteral("/cpuinfo_max_freq")).toInt();
        for (const QByteArray &tok : readFile(info.dir + QStringLiteral("/scaling_available_frequencies")).split(' ')) {
            const int f = tok.trimmed().toInt();
            if (f > 0)
                info.freqs.append(f);
        }
        std::sort(info.freqs.begin(), info.freqs.end());
        if (info.maxKHz > 0)
            m_policies.append(info);
    }
    std::sort(m_policies.begin(), m_policies.end(),
              [](const PolicyInfo &a, const PolicyInfo &b) { return a.firstCpu < b.firstCpu; });

    m_skinThermalPath = DeviceProfile::current().thermalPaths().skin;
    m_gpuLevels = readFile(QLatin1String(kKgsl) + QStringLiteral("num_pwrlevels")).toInt();
    qCInfo(lcPerf, "tablet topology: %d CPU clusters, %d GPU levels, skin=%s",
           int(m_policies.size()), m_gpuLevels, qPrintable(m_skinThermalPath));
}

int Perf::pickCeilingKHz(const PolicyInfo &p, double ratio) const
{
    if (ratio >= 0.99 || p.freqs.isEmpty())
        return p.maxKHz;
    const int target = int(p.maxKHz * ratio);
    int best = p.freqs.first();
    for (int f : p.freqs) {
        if (f <= target)
            best = f;
        else
            break;
    }
    return std::max(p.minKHz, best);
}

void Perf::saveTabletState() const
{
    QDir().mkpath(QStringLiteral("/userdata/naomi/state"));
    QSettings s(QLatin1String(kStore), QSettings::IniFormat);
    s.setValue(QStringLiteral("profile"), profile());
    s.setValue(QStringLiteral("stayAwake"), stayAwake());
    s.setValue(QStringLiteral("deepSuspend"), deepSuspend());
}

void Perf::applyTabletProfile()
{
    if (m_policies.isEmpty())
        return;

    const QString prof = profile();
    const int therm = thermalLevel();

    // Потолки по кластерам (доля от cpuinfo_max_freq: [Little, Big, Prime]):
    // На SM8250 3.19 ГГц у Prime потребляет вдвое больше Вт чем 2.65 ГГц — в Балансе держим 85%.
    double ratios[3] = {1.0, 0.90, 0.85};
    int sampleUs = 20000;
    int upThresh = 70;
    int ioBusy = 1;
    int downFactor = 2;
    QByteArray boostFreq = "0:1171200";
    int boostMs = 60;
    int gpuMaxLvl = 0;   // 0 = max freq in kgsl

    if (prof == QLatin1String("eco")) {
        ratios[0] = 0.75; ratios[1] = 0.65; ratios[2] = 0.55;
        sampleUs = 30000; upThresh = 85; ioBusy = 0; downFactor = 1;
        boostFreq = "0:0"; boostMs = 0;
        gpuMaxLvl = m_gpuLevels > 4 ? 4 : 0;   // ~441 MHz cap on Adreno 650
    } else if (prof == QLatin1String("performance")) {
        ratios[0] = 1.0; ratios[1] = 1.0; ratios[2] = 1.0;
        sampleUs = 10000; upThresh = 55; ioBusy = 1; downFactor = 4;
        boostFreq = "0:1516800"; boostMs = 120;
        gpuMaxLvl = 0;
    }

    // Ступенчатая термозащита по датчику корпуса (quiet_therm / skin-therm) поверх профиля
    if (therm >= 3) {
        ratios[0] = std::min(ratios[0], 0.75);
        ratios[1] = std::min(ratios[1], 0.55);
        ratios[2] = std::min(ratios[2], 0.45);
        gpuMaxLvl = std::max(gpuMaxLvl, m_gpuLevels > 5 ? 5 : 0);
        boostMs = 0;
    } else if (therm == 2) {
        ratios[0] = std::min(ratios[0], 0.90);
        ratios[1] = std::min(ratios[1], 0.70);
        ratios[2] = std::min(ratios[2], 0.60);
        gpuMaxLvl = std::max(gpuMaxLvl, m_gpuLevels > 4 ? 4 : 0);
        boostMs = std::min(boostMs, 40);
    } else if (therm == 1) {
        ratios[1] = std::min(ratios[1], 0.85);
        ratios[2] = std::min(ratios[2], 0.77);
        gpuMaxLvl = std::max(gpuMaxLvl, m_gpuLevels > 2 ? 2 : 0);
    }

    for (int i = 0; i < m_policies.size(); ++i) {
        const auto &p = m_policies.at(i);
        const double ratio = ratios[std::min(i, 2)];
        const QByteArray avail = readFile(p.dir + QStringLiteral("/scaling_available_governors"));
        const QByteArray gov = avail.contains("ondemand") ? QByteArrayLiteral("ondemand") : QByteArrayLiteral("schedutil");
        if (readFile(p.dir + QStringLiteral("/scaling_governor")) != gov)
            writeFile(p.dir + QStringLiteral("/scaling_governor"), gov, true);
        if (p.minKHz > 0)
            writeFile(p.dir + QStringLiteral("/scaling_min_freq"), QByteArray::number(p.minKHz), true);
        writeFile(p.dir + QStringLiteral("/scaling_max_freq"), QByteArray::number(pickCeilingKHz(p, ratio)), true);
        if (gov == "ondemand") {
            const QString od = p.dir + QStringLiteral("/ondemand/");
            writeFile(od + QStringLiteral("sampling_rate"), QByteArray::number(sampleUs), true);
            writeFile(od + QStringLiteral("up_threshold"), QByteArray::number(upThresh), true);
            writeFile(od + QStringLiteral("io_is_busy"), QByteArray::number(ioBusy), true);
            writeFile(od + QStringLiteral("sampling_down_factor"), QByteArray::number(downFactor), true);
        }
    }

    writeFile(QLatin1String(kCpuBoost) + QStringLiteral("input_boost_freq"), boostFreq, true);
    writeFile(QLatin1String(kCpuBoost) + QStringLiteral("input_boost_ms"), QByteArray::number(boostMs), true);

    if (m_gpuLevels > 0) {
        writeFile(QLatin1String(kKgsl) + QStringLiteral("force_no_nap"), "0", true);
        writeFile(QLatin1String(kKgsl) + QStringLiteral("max_pwrlevel"),
                  QByteArray::number(std::clamp(gpuMaxLvl, 0, m_gpuLevels - 1)), true);
        writeFile(QLatin1String(kKgsl) + QStringLiteral("min_pwrlevel"),
                  QByteArray::number(m_gpuLevels - 1), true);
    }
}

void Perf::pollThermal()
{
    if (m_skinThermalPath.isEmpty())
        return;
    const double tempC = readFile(m_skinThermalPath).toDouble() / 1000.0;
    if (tempC <= 0.0 || tempC > 120.0)
        return;
    set("SkinTempC", qRound(tempC * 10.0) / 10.0);

    const int cur = thermalLevel();
    int next = cur;
    // Пороги с гистерезисом 2 °C (вход 39 / 42 / 45 °C, выход 37 / 40 / 43 °C)
    if (tempC >= 45.0)
        next = 3;
    else if (tempC >= 42.0)
        next = std::max(cur == 3 && tempC > 43.0 ? 3 : 2, 2);
    else if (tempC >= 39.0)
        next = (cur >= 2 && tempC > 40.0) ? 2 : 1;
    else if (tempC < 37.0)
        next = 0;
    else if (cur >= 2 && tempC <= 40.0)
        next = 1;

    if (next != cur) {
        set("ThermalLevel", next);
        qCInfo(lcPerf, "термозащита корпуса: %.1f °C -> уровень %d", tempC, next);
        applyTabletProfile();
    }
}

void Perf::updateWakeLock()
{
    if (!DeviceProfile::current().tablet)
        return;
    const bool plugged = usbPlugged();
    const bool needLock = m_displayOn || stayAwake() || plugged;
    if (needLock) {
        m_suspendTimer.stop();
        if (!m_wakeLockHeld) {
            if (writeFile(QStringLiteral("/sys/power/wake_lock"), kWakeLockName, true))
                m_wakeLockHeld = true;
        }
        return;
    }
    if (deepSuspend()) {
        if (!m_suspendTimer.isActive())
            m_suspendTimer.start();
    } else if (!m_wakeLockHeld) {
        if (writeFile(QStringLiteral("/sys/power/wake_lock"), kWakeLockName, true))
            m_wakeLockHeld = true;
    }
}

void Perf::tryDeepSuspend()
{
    if (!DeviceProfile::current().tablet || !deepSuspend())
        return;
    if (m_displayOn || stayAwake() || usbPlugged()) {
        updateWakeLock();
        return;
    }
    if (hasActiveSshSession()) {
        qCInfo(lcPerf, "deep-сон отложен: активна SSH-сессия");
        m_suspendTimer.start(30000);
        return;
    }
    if (m_wakeLockHeld) {
        writeFile(QStringLiteral("/sys/power/wake_unlock"), kWakeLockName, true);
        m_wakeLockHeld = false;
    }
    qCInfo(lcPerf, "экран выключен от батареи — уходим в deep-сон (mem)");
    const bool ok = writeFile(QStringLiteral("/sys/power/state"), "mem", false);
    // Сюда возвращаемся после пробуждения!
    qCInfo(lcPerf, "выход из deep-сна (%s)", ok ? "ok" : "ошибка");
    if (writeFile(QStringLiteral("/sys/power/wake_lock"), kWakeLockName, true))
        m_wakeLockHeld = true;
    applyTabletProfile();
    updateWakeLock();
}

void Perf::SetProfile(const QString &raw)
{
    if (!DeviceProfile::current().tablet)
        return;
    const QString p = normalizeProfile(raw);
    if (p == profile())
        return;
    set("Profile", p);
    saveTabletState();
    applyTabletProfile();
    qCInfo(lcPerf, "профиль питания: %s", qPrintable(p));
}

void Perf::SetStayAwake(bool on)
{
    if (!DeviceProfile::current().tablet || on == stayAwake())
        return;
    set("StayAwake", on);
    saveTabletState();
    updateWakeLock();
    qCInfo(lcPerf, "режим сервера (не усыплять сеть): %s", on ? "вкл." : "выкл.");
}

void Perf::SetDeepSuspend(bool on)
{
    if (!DeviceProfile::current().tablet || on == deepSuspend())
        return;
    set("DeepSuspend", on);
    saveTabletState();
    updateWakeLock();
    qCInfo(lcPerf, "глубокий сон (mem): %s", on ? "вкл." : "выкл.");
}

void Perf::SetDisplayOn(bool on)
{
    if (!DeviceProfile::current().tablet)
        return;
    m_displayOn = on;
    writeFile(QLatin1String(kScreenState), on ? "1" : "0", true);
    if (on) {
        if (!m_thermalTimer.isActive())
            m_thermalTimer.start();
        pollThermal();
    } else if (!usbPlugged()) {
        m_thermalTimer.stop();
    }
    updateWakeLock();
}

void Perf::HoldDesk(bool on)
{
    if (!DeviceProfile::current().pixel || !calledFromDBus())
        return;
    const QString who = message().service();
    if (on) {
        m_holders.insert(who);
        if (!m_watch.watchedServices().contains(who))
            m_watch.addWatchedService(who);
        m_shellPid = bus().interface()->servicePid(who).value();
    } else {
        m_holders.remove(who);
        m_watch.removeWatchedService(who);
    }
    apply();
}

void Perf::apply()
{
    const bool on = !m_holders.isEmpty();
    if (on == desk())
        return;
    floors(on);
    awake(on);
    boostShell(on);
    boostApps(on);
    m_gpu->holdTop(on);
    skin(on);
    set("Desk", on);
    qCInfo(lcPerf, "стол на полную: %s", on ? "да — пол частот ядер, ядра без глубокого сна, оболочка и Android "
                                                 "подняты, видеочип 625 МГц, пороги корпуса +4 °C"
                                            : "нет — частоты, сон ядер, видеочип и пороги корпуса свои");
}

void Perf::floors(bool on)
{
    for (const auto &cluster : DeviceProfile::current().clusters) {
        const QString dir = QLatin1String(kCpufreq) + QStringLiteral("policy%1").arg(cluster.firstCpu);
        if (!QDir(dir).exists())
            continue;
        const QByteArray value = on ? QByteArray::number(cluster.deskFloorKHz) : readFile(dir + "/cpuinfo_min_freq");
        if (!value.isEmpty())
            writeFile(dir + "/scaling_min_freq", value);
    }
}

void Perf::awake(bool on)
{
    if (!on) {
        m_cpuLatency.close();
        return;
    }
    if (m_cpuLatency.isOpen())
        return;
    m_cpuLatency.setFileName(QLatin1String(kCpuLatency));
    if (!m_cpuLatency.open(QIODevice::WriteOnly | QIODevice::Unbuffered)
        || m_cpuLatency.write(reinterpret_cast<const char *>(&kCpuLatencyUs), sizeof kCpuLatencyUs) != sizeof kCpuLatencyUs) {
        qCWarning(lcPerf, "%s: %s — ядра спят как обычно", kCpuLatency, qPrintable(m_cpuLatency.errorString()));
        m_cpuLatency.close();
    }
}

void Perf::boostShell(bool on)
{
    const QString group = QLatin1String(kShellGroup);
    if (on) {
        if (!QFile::exists(QLatin1String(kStune) + "/schedtune.boost")) {
            qCWarning(lcPerf, "schedtune не подключён (%s) — оболочка без подъёма", kStune);
            return;
        }
        QDir().mkpath(group);
        if (m_shellPid)
            writeFile(group + "/cgroup.procs", QByteArray::number(m_shellPid));
    } else if (!QFile::exists(group)) {
        return;
    }
    writeFile(group + "/schedtune.prefer_idle", on ? "1" : "0");
    writeFile(group + "/schedtune.prefer_high_cap", on ? "1" : "0");
    writeFile(group + "/schedtune.boost", on ? kShellBoost : "0");
    if (m_shellPid && setpriority(PRIO_PROCESS, id_t(m_shellPid), on ? kShellNice : 0) != 0)
        qCWarning(lcPerf, "приоритет потока интерфейса оболочки (%u): %s", m_shellPid, strerror(errno));
}

void Perf::boostApps(bool on)
{
    const QString group = QLatin1String(kAppGroup);
    if (!QFile::exists(QLatin1String(kStune) + "/schedtune.boost"))
        return;
    if (on)
        QDir().mkpath(group);
    else if (!QFile::exists(group))
        return;
    writeFile(group + "/schedtune.prefer_idle", on ? "1" : "0");
    writeFile(group + "/schedtune.prefer_high_cap", on ? "1" : "0");
    writeFile(group + "/schedtune.boost", on ? kAppBoost : "0");
}

void Perf::skin(bool on)
{
    if (QFile::exists(QLatin1String(kSkinCpuOffset)))
        writeFile(QLatin1String(kSkinCpuOffset), on ? kSkinCpuShift : "0");
}
