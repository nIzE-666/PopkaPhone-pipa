#include "gpu.h"
#include "device_profile.h"

#include <QFile>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcGpu, "naomid.gpu")

namespace {

const char kDir[] = "/sys/class/kgsl/kgsl-3d0/";
// уровни kgsl на SM7250: 0 — 625 МГц, 1 — 500, 2 — 400, 3 — 275 (gpu_available_frequencies)
constexpr int kFastLevel = 1;

int readInt(const char *name)
{
    QFile f(QString::fromLatin1(kDir) + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    bool ok = false;
    const int v = f.readAll().trimmed().toInt(&ok);
    return ok ? v : -1;
}

} // namespace

Gpu::Gpu(QObject *parent)
    : BusObject("/org/naomi/Phone/Gpu", "org.naomi.Phone.Gpu", parent)
{
    set("Fast", false);
    if (!DeviceProfile::current().pixel)
        return;  // ступени SM7250 не определяют поведение Adreno 650
    // «свой» уровень — не то, что стоит сейчас (служба могла упасть, держа границу), а самый низкий
    const int levels = readInt("num_pwrlevels");
    m_lowest = levels > 0 ? levels - 1 : -1;
    m_watch.setConnection(bus());
    m_watch.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watch, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &who) {
        m_watch.removeWatchedService(who);
        if (m_holders.remove(who)) {
            qCInfo(lcGpu, "видеочип: %s ушёл с шины — отпускаю", qPrintable(who));
            apply();
        }
    });
    apply();
}

void Gpu::HoldFast(bool on)
{
    const QString who = calledFromDBus() ? message().service() : QStringLiteral("naomid");
    if (on) {
        m_holders.insert(who);
        if (calledFromDBus() && !m_watch.watchedServices().contains(who))
            m_watch.addWatchedService(who);
    } else {
        m_holders.remove(who);
        m_watch.removeWatchedService(who);
    }
    apply();
}

void Gpu::holdTop(bool on)
{
    if (on == m_top)
        return;
    m_top = on;
    apply();
}

void Gpu::apply()
{
    const bool fast = m_top || !m_holders.isEmpty();
    if (m_lowest < 0)
        return;
    const int level = m_top ? 0 : fast ? kFastLevel : m_lowest;
    QFile f(QString::fromLatin1(kDir) + QLatin1String("min_pwrlevel"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QByteArray::number(level)) < 0) {
        qCWarning(lcGpu, "видеочип: min_pwrlevel %d не записан: %s", level, qPrintable(f.errorString()));
        return;
    }
    if (fast != prop("Fast").toBool() || m_top != m_topSet)
        qCInfo(lcGpu, "видеочип: не ниже %s", m_top ? "625 МГц" : fast ? "500 МГц" : "своего (275 МГц)");
    m_topSet = m_top;
    set("Fast", fast);
}
