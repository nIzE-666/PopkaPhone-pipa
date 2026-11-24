#include "perf.h"

#include "device_profile.h"
#include "gpu.h"

#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>

#include <cerrno>
#include <cstring>
#include <sys/resource.h>

Q_LOGGING_CATEGORY(lcPerf, "naomid.perf")

namespace {

const char kCpufreq[] = "/sys/devices/system/cpu/cpufreq/";
// группа планировщика оболочки: schedtune подключает shell-prepare (/dev/stune)
const char kStune[] = "/dev/stune";
const char kShellGroup[] = "/dev/stune/shell";
const char kShellBoost[] = "20";
const int kShellNice = -5;   // поток интерфейса оболочки в режиме стола (boostShell)
// Приложение Android на переднем плане (группа top-app — её держит сам Android в контейнере Waydroid; программ
// телефона в ней нет): подъём — как у Pixel 5 в покое (10), будить на свободном ядре, на больших ядрах — как у Android
// при касании (/vendor/etc/powerhint.json: TASchedtuneBoost 30/10, TASchedtuneHighCap 1/0). Подсказок касаний
// Android в контейнере никто не шлёт, и игра без этого жила на малых ядрах (docs/ANDROID.md, «Скорость»)
const char kAppGroup[] = "/dev/stune/top-app";
const char kAppBoost[] = "10";
// Защита «корпус» для процессора (зона skin-therm-cpu: 37, 39, 41, 43 °C — ступени наибольшей частоты ядер) в режиме
// стола — на 4 °C позже: 41, 43, 45, 47 (выбор пользователя 17 ноября 2026: телефон лежит на столе, а не в руке).
// offset зоны прибавляется к её показанию (так же сдвигает пороги Android — powerhint.json, SkinThermCpuTzOffset).
// Зону skin-therm (ток зарядки с 36 °C, ядра и видеочип на 45 и 47 °C) и плату sdm-therm не трогаем
const char kSkinCpuOffset[] = "/sys/devices/virtual/thermal/tz-by-name/skin-therm-cpu/offset";
const char kSkinCpuShift[] = "-4000";
// Сон ядер в режиме стола — только самый мелкий (C0, выход 60 мкс): запрос PM QoS — число мкс в открытом файле,
// действует, пока файл открыт (служба упала — ядро снимает само). Глубокий сон (C1, C2) у SM7250 — выход 0,9 мс на
// малых ядрах и 1,2–1,9 мс на больших, а кадр стола — цепочка коротких пробуждений раз в 13–17 мс (прерывание кабеля →
// sshd → поток трансляции → поток интерфейса → видеочип → кодер → сеть): между ними ядра успевали уснуть, и каждое
// звено ждало, пока ядро проснётся. Рентген 19 ноября 2026 (docs/XRAY.md): поток трансляции 82 % времени не работал,
// а ждал ядро (ожидание после пробуждения 0,7/1,3 мс, медиана/95 %), поток интерфейса — 1,0 мс на каждое пробуждение.
// Замер (tools/deskbench, по два прогона): от такта до распакованного на приставке 26,9 → 24,1 мс, такт до потока
// интерфейса (95 %) 4,0 → 1,1–1,7 мс, опорный кадр 47 → 42 мс; цена в покое +0,05–0,12 Вт (1,08 → 1,14–1,19)
const char kCpuLatency[] = "/dev/cpu_dma_latency";
const qint32 kCpuLatencyUs = 100;

bool write(const QString &path, const QByteArray &value)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly) && f.write(value) == value.size())
        return true;
    qCWarning(lcPerf, "%s ← %s: %s", qPrintable(path), value.constData(), qPrintable(f.errorString()));
    return false;
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}

} // namespace

Perf::Perf(Gpu *gpu, QObject *parent)
    : BusObject("/org/naomi/Phone/Perf", "org.naomi.Phone.Perf", parent)
    , m_gpu(gpu)
{
    set("Desk", false);
    if (!DeviceProfile::current().pixel)
        return;
    m_watch.setConnection(bus());
    m_watch.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watch, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &who) {
        m_watch.removeWatchedService(who);
        if (m_holders.remove(who)) {
            qCInfo(lcPerf, "стол на полную: %s ушёл с шины — отпускаю", qPrintable(who));
            apply();
        }
    });
    floors(false);   // служба могла упасть, держа
    skin(false);
    boostApps(false);
    if (QFile::exists(QLatin1String(kShellGroup)))
        boostShell(false);
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
        const QByteArray value = on ? QByteArray::number(cluster.deskFloorKHz) : read(dir + "/cpuinfo_min_freq");
        if (!value.isEmpty())
            write(dir + "/scaling_min_freq", value);
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
            write(group + "/cgroup.procs", QByteArray::number(m_shellPid));
    } else if (!QFile::exists(group)) {
        return;
    }
    // prefer_idle — будить на свободном ядре, а не ставить в очередь к занятому; prefer_high_cap — из свободных брать
    // большие: без него поток интерфейса стола полжизни шёл на малых A55 (свободных больше) — шаг растягивания окна
    // 9 мс на ядре 0 против 3 на ядре 7, кадр стола (95 %) вдвое дольше (прогон стола 19 ноября 2026: долгих событий
    // на малых ядрах 162 → 1, docs/DESKTOP.md, «Прогон стола»). Без режима — как у всех
    write(group + "/schedtune.prefer_idle", on ? "1" : "0");
    write(group + "/schedtune.prefer_high_cap", on ? "1" : "0");
    write(group + "/schedtune.boost", on ? kShellBoost : "0");
    // Поток интерфейса оболочки (главный: его tid — pid процесса) — вперёд программ стола: на больших ядрах с ним
    // Chromium (браузер листают — оба больших ядра заняты), и такт ждал своей очереди до 15 мс (прогон стола
    // 19 ноября 2026: «в очереди за ядром»). Остальные потоки оболочки — как были
    if (m_shellPid && setpriority(PRIO_PROCESS, id_t(m_shellPid), on ? kShellNice : 0) != 0)
        qCWarning(lcPerf, "приоритет потока интерфейса оболочки (%u): %s", m_shellPid, strerror(errno));
}

// Группу top-app создаёт init Android при включении контейнера (mkdir, chown — значений не пишет), а режим стола
// включается раньше — после загрузки телефона группы ещё нет. Поэтому служба создаёт её сама: mkdir Android тогда
// ничего не меняет, подъём остаётся (проверено 18 ноября 2026 после перезагрузки)
void Perf::boostApps(bool on)
{
    const QString group = QLatin1String(kAppGroup);
    if (!QFile::exists(QLatin1String(kStune) + "/schedtune.boost"))
        return;
    if (on)
        QDir().mkpath(group);
    else if (!QFile::exists(group))
        return;
    write(group + "/schedtune.prefer_idle", on ? "1" : "0");
    write(group + "/schedtune.prefer_high_cap", on ? "1" : "0");
    write(group + "/schedtune.boost", on ? kAppBoost : "0");
}

void Perf::skin(bool on)
{
    if (QFile::exists(QLatin1String(kSkinCpuOffset)))
        write(QLatin1String(kSkinCpuOffset), on ? kSkinCpuShift : "0");
}
