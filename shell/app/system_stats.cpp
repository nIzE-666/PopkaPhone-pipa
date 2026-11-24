#include "system_stats.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace {

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()) : QString();
}

// вид сети по имени интерфейса: wlan — Wi-Fi, usb/rndis — кабель разработчика, rmnet — мобильная
QString kindOf(const QString &iface)
{
    if (iface.startsWith("wl"))
        return "wifi";
    if (iface.startsWith("usb") || iface.startsWith("rndis"))
        return "cable";
    if (iface.startsWith("rmnet"))
        return "mobile";
    return {};
}

} // namespace

QVariantMap SystemStats::sample()
{
    QVariantMap out;

    // память: занято = всего − доступно (MemAvailable — то, что ядро отдаст без подкачки)
    quint64 total = 0, avail = 0;
    for (const QString &line : readAll("/proc/meminfo").split('\n')) {
        const QStringList f = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (f.size() >= 2 && f[0] == "MemTotal:")
            total = f[1].toULongLong() * 1024;
        else if (f.size() >= 2 && f[0] == "MemAvailable:")
            avail = f[1].toULongLong() * 1024;
    }
    out["memTotal"] = double(total);
    out["memUsed"] = double(total > avail ? total - avail : 0);

    // процессор: доля не-простоя (всё, кроме idle и iowait) за время с прошлого замера
    const QStringList cpu = readAll("/proc/stat").section('\n', 0, 0).split(' ', Qt::SkipEmptyParts);
    quint64 all = 0, idle = 0;
    for (int i = 1; i < cpu.size(); ++i) {
        const quint64 v = cpu[i].toULongLong();
        all += v;
        if (i == 4 || i == 5)
            idle += v;
    }
    const quint64 busy = all - idle;
    out["cpuLoad"] = m_cpuAll && all > m_cpuAll ? 100.0 * (busy - m_cpuBusy) / (all - m_cpuAll) : -1.0;
    m_cpuBusy = busy;
    m_cpuAll = all;
    int cores = 0;
    for (const QString &line : readAll("/proc/stat").split('\n'))
        cores += QRegularExpression("^cpu\\d+ ").match(line).hasMatch();
    out["cores"] = cores;

    // частоты: кластер — каталог policyN (у Pixel 5: 0 — шесть малых ядер, 6 — большое, 7 — главное)
    QVariantList freqs;
    const QDir pol("/sys/devices/system/cpu/cpufreq");
    QStringList names = pol.entryList({"policy*"}, QDir::Dirs);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return a.mid(6).toInt() < b.mid(6).toInt(); });
    for (const QString &p : names)
        freqs << readAll(pol.filePath(p + "/scaling_cur_freq")).trimmed().toDouble() / 1e6;
    out["freqs"] = freqs;

    // адреса IPv4 по интерфейсам
    QVariantList nets;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
                continue;
            const QString iface = QString::fromLatin1(a->ifa_name);
            const QString kind = kindOf(iface);
            if (kind.isEmpty())
                continue;
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, buf, sizeof buf);
            nets << QVariantMap{{"kind", kind}, {"iface", iface}, {"ip", QString::fromLatin1(buf)}};
        }
        freeifaddrs(list);
    }
    out["nets"] = nets;

    // трафик: байты всех внешних интерфейсов (Wi-Fi, кабель, мобильная) за время с прошлого замера
    quint64 rx = 0, tx = 0;
    for (const QString &line : readAll("/proc/net/dev").split('\n')) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString iface = line.left(colon).trimmed();
        if (kindOf(iface).isEmpty() || iface.startsWith("r_"))
            continue;
        const QStringList f = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        if (f.size() >= 9) {
            rx += f[0].toULongLong();
            tx += f[8].toULongLong();
        }
    }
    const qint64 ms = m_since.isValid() ? m_since.restart() : (m_since.start(), 0);
    out["rxRate"] = ms > 0 && rx >= m_rx ? (rx - m_rx) * 1000.0 / ms : -1.0;
    out["txRate"] = ms > 0 && tx >= m_tx ? (tx - m_tx) * 1000.0 / ms : -1.0;
    m_rx = rx;
    m_tx = tx;
    return out;
}
