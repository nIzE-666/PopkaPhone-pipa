#include "system_monitor.h"

#include "desktop_screen.h"
#include "device_profile.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

Q_LOGGING_CATEGORY(lcMonitor, "naomi.monitor")

namespace {

QByteArray slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// значение строки «ключ число» (meminfo, vmstat, cpu.stat, memory.stat); нет — 0
quint64 field(const QByteArray &text, const char *key)
{
    const QByteArray k = QByteArray(key) + ' ';
    int at = text.startsWith(k) ? 0 : text.indexOf('\n' + k);
    if (at < 0)
        return 0;
    at += (at ? 1 : 0) + k.size();
    while (at < text.size() && (text[at] == ' ' || text[at] == ':'))
        ++at;
    quint64 v = 0;
    for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at)
        v = v * 10 + quint64(text[at] - '0');
    return v;
}

// строка /proc/stat одного ядра (или всех): занято и всего, тики
bool cpuLine(const QByteArray &stat, const QByteArray &name, quint64 &busy, quint64 &all)
{
    const int at = stat.startsWith(name + ' ') ? 0 : stat.indexOf('\n' + name + ' ');
    if (at < 0)
        return false;
    const QList<QByteArray> f = stat.mid(at, stat.indexOf('\n', at + 1) - at).simplified().split(' ');
    if (f.size() < 8)
        return false;
    all = 0;
    for (int i = 1; i < 9 && i < f.size(); ++i)
        all += f[i].toULongLong();
    busy = all - f[4].toULongLong() - f[5].toULongLong();   // без простоя и ожидания диска
    return true;
}

const char kCgroup[] = "/sys/fs/cgroup/lxc.payload.waydroid";
const QColor kBad(0xd9, 0x70, 0x5f), kAmber(0xd8, 0xb2, 0x5a);

} // namespace

SystemMonitor::SystemMonitor(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(1000);
    m_timer.setTimerType(Qt::PreciseTimer);   // ровно секунда: «кадров в секунду» — без поправок
    connect(&m_timer, &QTimer::timeout, this, [this] { sample(false); });
}

void SystemMonitor::setActive(bool on)
{
    if (on == m_timer.isActive())
        return;
    if (m_desk)
        m_desk->setPulse(on);
    if (on) {
        const auto &profile = DeviceProfile::current();
        if (profile.tablet) {
            const auto paths = profile.thermalPaths();
            m_skinPath = paths.skin;
            m_cpuZones = paths.cpu;
        } else if (m_skinPath.isEmpty()) {   // зоны тепла — по именам, один раз
            const QDir zones(QStringLiteral("/sys/class/thermal"));
            for (const QString &z : zones.entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs)) {
                const QByteArray type = slurp(zones.filePath(z + "/type")).trimmed();
                if (type == "skin-therm")
                    m_skinPath = zones.filePath(z + "/temp");
                else if (type == "cpu-1-0-usr")
                    m_chipPath = zones.filePath(z + "/temp");
            }
        }
        m_count = m_head = 0;   // прошлая минута — давно: график с чистого листа
        sample(true);
        m_timer.start();
    } else {
        m_timer.stop();
    }
    emit activeChanged();
}

void SystemMonitor::push(const std::array<float, LaneCount> &v)
{
    for (int lane = 0; lane < LaneCount; ++lane)
        m_hist[lane][m_head] = v[lane];
    m_head = (m_head + 1) % kSeconds;
    m_count = std::min(m_count + 1, kSeconds);
}

// first — открыли только что: запомнить счётчики, цифры — со следующего замера
void SystemMonitor::sample(bool first)
{
    const double dt = first || !m_since.isValid() ? 1.0 : std::max(0.2, m_since.nsecsElapsed() / 1e9);
    m_since.start();
    QVariantMap now;
    std::array<float, LaneCount> lane{};

    // кадры стола
    DesktopScreen::Pulse pulse;
    if (m_desk) {
        pulse = m_desk->takePulse();
        now["hz"] = m_desk->rateHz();
        now["width"] = m_desk->pixelSize().width();
        now["height"] = m_desk->pixelSize().height();
    }
    now["fps"] = qRound(pulse.frames / dt);
    now["frameMs"] = pulse.medianMs;
    now["worstMs"] = pulse.worstMs;
    lane[Frames] = float(pulse.worstMs);
    lane[Missed] = float(pulse.missed);

    // процессор: все ядра и главное (последнее)
    const QByteArray stat = slurp(QStringLiteral("/proc/stat"));
    quint64 busy = 0, all = 0;
    double cpu = 0, prime = 0;
    if (cpuLine(stat, "cpu", busy, all)) {
        if (all > m_cpuAll)
            cpu = 100.0 * double(busy - m_cpuBusy) / double(all - m_cpuAll);
        m_cpuBusy = busy;
        m_cpuAll = all;
    }
    for (int n = 15; n >= 0; --n)
        if (cpuLine(stat, "cpu" + QByteArray::number(n), busy, all)) {
            if (all > m_primeAll)
                prime = 100.0 * double(busy - m_primeBusy) / double(all - m_primeAll);
            m_primeBusy = busy;
            m_primeAll = all;
            break;
        }
    now["cpu"] = cpu;
    now["prime"] = prime;
    lane[Cpu] = float(cpu);

    // видеочип
    const QList<QByteArray> gb = slurp(QStringLiteral("/sys/class/kgsl/kgsl-3d0/gpubusy")).simplified().split(' ');
    const double gpu = gb.size() == 2 && gb[1].toDouble() > 0 ? 100.0 * gb[0].toDouble() / gb[1].toDouble() : 0;
    now["gpu"] = gpu;
    now["gpuMhz"] = qRound(slurp(QStringLiteral("/sys/class/kgsl/kgsl-3d0/gpuclk")).trimmed().toDouble() / 1e6);
    lane[Gpu] = float(gpu);

    // память и диск
    const QByteArray mem = slurp(QStringLiteral("/proc/meminfo"));
    const double total = field(mem, "MemTotal:") / 1048576.0;
    now["memTotalGb"] = total;
    now["memGb"] = total - field(mem, "MemAvailable:") / 1048576.0;
    const QByteArray vm = slurp(QStringLiteral("/proc/vmstat"));
    const quint64 pages = field(vm, "pgpgin") + field(vm, "pgpgout");   // КБ
    const double disk = pages > m_pages ? double(pages - m_pages) / 1024.0 / dt : 0;
    m_pages = pages;
    now["diskMb"] = disk;
    lane[Disk] = float(disk);

    // тепло
    const double skin = slurp(m_skinPath).trimmed().toDouble() / 1000.0;
    now["skin"] = skin;
    now["chip"] = m_cpuZones.isEmpty() ? slurp(m_chipPath).trimmed().toDouble() / 1000.0 : maxTemperature(m_cpuZones);
    bool hot = false;
    const auto &clusters = DeviceProfile::current().clusters;
    for (const auto &cluster : {clusters[1], clusters[2]}) {
        const QString base = QStringLiteral("/sys/devices/system/cpu/cpufreq/policy%1/").arg(cluster.firstCpu);
        const qint64 cap = slurp(base + "scaling_max_freq").trimmed().toLongLong();
        hot = hot || (cap > 0 && cap < slurp(base + "cpuinfo_max_freq").trimmed().toLongLong());
    }
    now["hot"] = hot;
    lane[Temp] = float(skin);

    // Android: группа процессов Waydroid
    const quint64 used = field(slurp(QString::fromLatin1(kCgroup) + "/cpu.stat"), "usage_usec");
    const double android = used > m_androidUs ? double(used - m_androidUs) / 1e4 / dt : 0;
    m_androidUs = used;
    const QByteArray ms = slurp(QString::fromLatin1(kCgroup) + "/memory.stat");
    now["android"] = android;
    now["androidGb"] = double(field(ms, "anon") + field(ms, "shmem")) / 1073741824.0;
    lane[Android] = float(android);

    if (first)
        return;
    push(lane);
    int missed = 0;
    for (int i = 0; i < m_count; ++i)
        missed += int(at(Missed, i));
    now["missed"] = missed;
    m_now = now;
    // для сверки с прогоном стола: QT_LOGGING_RULES=naomi.monitor.debug=true
    qCDebug(lcMonitor, "кадров %d, кадр %.1f мс (худший %.1f), пропусков %d; процессор %.0f %%, видеочип %.0f %%, "
            "диск %.0f МБ/с, Android %.0f %%", pulse.frames, pulse.medianMs, pulse.worstMs, pulse.missed, cpu, gpu, disk,
            android);
    emit sampled();
}

MonitorGraph::MonitorGraph(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    connect(this, &MonitorGraph::changed, this, [this] { update(); });
}

void MonitorGraph::setMonitor(SystemMonitor *monitor)
{
    if (monitor == m_monitor)
        return;
    if (m_monitor)
        disconnect(m_monitor, nullptr, this, nullptr);
    m_monitor = monitor;
    if (m_monitor)
        connect(m_monitor, &SystemMonitor::sampled, this, [this] { update(); });
    emit changed();
}

void MonitorGraph::paint(QPainter *p)
{
    const double w = width(), h = height();
    if (!m_monitor || w <= 0 || h <= 0 || m_max <= m_min)
        return;
    const int n = m_monitor->count();
    const double step = w / (SystemMonitor::kSeconds - 1);
    auto xOf = [&](int i) { return w - (n - 1 - i) * step; };
    auto yOf = [&](double v) { return h - 1 - std::clamp((v - m_min) / (m_max - m_min), 0.0, 1.0) * (h - 2); };
    p->setRenderHint(QPainter::Antialiasing, true);
    if (m_limit > m_min && m_limit < m_max) {
        QColor c = kAmber;
        c.setAlphaF(0.45);
        p->setPen(QPen(c, 1, Qt::CustomDashLine, Qt::FlatCap));
        QPen pen = p->pen();
        pen.setDashPattern({2, 3});
        p->setPen(pen);
        const double y = std::round(yOf(m_limit)) + 0.5;
        p->drawLine(QPointF(0, y), QPointF(w, y));
    }
    if (m_marks) {
        QColor c = kBad;
        c.setAlphaF(m_marks > 1 ? 0.9 : 0.45);
        p->setPen(QPen(c, 1, Qt::SolidLine, Qt::FlatCap));
        for (int i = 0; i < n; ++i)
            if (m_monitor->at(SystemMonitor::Missed, i) > 0) {
                const double x = std::round(xOf(i)) - 0.5;
                p->drawLine(QPointF(x, 0), QPointF(x, h));
            }
    }
    if (n < 1)
        return;
    QPolygonF line;
    line.reserve(n);
    for (int i = 0; i < n; ++i)
        line << QPointF(xOf(i), yOf(m_monitor->at(m_lane, i)));
    if (n > 1) {
        QPolygonF area = line;
        area << QPointF(w, h) << QPointF(xOf(0), h);
        QColor fill = m_color;
        fill.setAlphaF(0.13);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawPolygon(area);
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(m_color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPolyline(line);
    }
    p->setPen(Qt::NoPen);
    p->setBrush(m_color);
    p->drawEllipse(QPointF(std::min(line.last().x(), w - 2), std::clamp(line.last().y(), 2.0, h - 2)), 2, 2);
}
