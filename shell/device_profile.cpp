#include "device_profile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

const DeviceProfile redfin{"redfin", false, true, 2.75, 432, 0, 90, {{60, 0}, {90, 1}},
                           {{{0, 6, 1363200}, {6, 1, 1478400}, {7, 1, 1766400}}}};
const DeviceProfile pipa{"pipa", true, false, 2.0, 309, 90, 60,
                         {},
                         {{{0, 4, 0}, {4, 3, 0}, {7, 1, 0}}}};
const DeviceProfile unknown{"unknown", false, false, redfin.density, redfin.panelDpi, 0, 60, {},
                            redfin.clusters};

const DeviceProfile *byName(QString name)
{
    name.remove(u'\0');
    name = name.simplified().toLower();
    static const QRegularExpression pipaWord(QStringLiteral(R"((?:^|[^a-z0-9_-])pipa(?:$|[^a-z0-9_-]))"));
    static const QRegularExpression redfinWord(QStringLiteral(R"((?:^|[^a-z0-9_-])redfin(?:$|[^a-z0-9_-]))"));
    if (pipaWord.match(name).hasMatch() || name == QLatin1String("xiaomi pad 6"))
        return &pipa;
    if (redfinWord.match(name).hasMatch() || name == QLatin1String("pixel 5"))
        return &redfin;
    return nullptr;
}

const DeviceProfile &detect()
{
    if (const auto *profile = byName(qEnvironmentVariable("NAOMI_DEVICE")))
        return *profile;

    const QString infoPath = QStringLiteral("/etc/deviceinfo");
    const QByteArray info = QFileInfo(infoPath).isFile() ? readDeviceFile(infoPath) : QByteArray();
    static const QRegularExpression entry(QStringLiteral(
        R"(^\s*(?:export\s+)?(?:deviceinfo_(?:codename|name)|Device(?:Name|Model|Codename)|Name|PrettyName)\s*=\s*([^#]+))"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QByteArray &line : info.split('\n')) {
        const auto match = entry.match(QString::fromUtf8(line));
        if (match.hasMatch()) {
            QString value = match.captured(1).trimmed();
            if ((value.startsWith(u'"') && value.endsWith(u'"')) || (value.startsWith(u'\'') && value.endsWith(u'\'')))
                value = value.mid(1, value.size() - 2);
            if (const auto *profile = byName(value))
                return *profile;
        }
    }

    for (const char *path : {"/proc/device-tree/model", "/sys/firmware/devicetree/base/model"})
        if (const auto *profile = byName(QString::fromUtf8(readDeviceFile(QLatin1String(path)))))
            return *profile;

    const QString getprop = QStandardPaths::findExecutable(QStringLiteral("getprop"));
    if (!getprop.isEmpty()) {
        // Один вызов: HAL может задавать ro.product.device и ro.product.vendor.device по-разному.
        QProcess process;
        process.start(getprop, {});
        if (process.waitForFinished(750)) {
            static const QRegularExpression prop(QStringLiteral(
                R"(^\[ro\.product\.(?:(?:vendor|odm)\.)?(?:device|model)\]: \[([^\]]+)\])"));
            for (const QByteArray &line : process.readAllStandardOutput().split('\n')) {
                const auto match = prop.match(QString::fromUtf8(line));
                if (match.hasMatch())
                    if (const auto *profile = byName(match.captured(1)))
                        return *profile;
            }
        } else {
            process.kill();
            process.waitForFinished();
        }
    }

    static const QRegularExpression channelDevice(
        QStringLiteral(R"(^\s*device\s*:\s*([a-z0-9_-]+)\s*(?:[#;].*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QByteArray &line : readDeviceFile(QStringLiteral("/etc/system-image/channel.ini")).split('\n')) {
        const auto match = channelDevice.match(QString::fromUtf8(line));
        if (match.hasMatch())
            if (const auto *profile = byName(match.captured(1)))
                return *profile;
    }
    return unknown;
}

} // namespace

const DeviceProfile &DeviceProfile::current()
{
    static const auto &profile = detect();
    return profile;
}

QString DeviceProfile::backlight() const
{
    const QString pipaLcd = QStringLiteral("/sys/class/backlight/4-0011/");
    if (tablet && QFile::exists(pipaLcd + "brightness"))
        return pipaLcd;
    return QStringLiteral("/sys/class/backlight/panel0-backlight/");
}

QString DeviceProfile::battery() { return QStringLiteral("/sys/class/power_supply/battery/"); }
QString DeviceProfile::usbSupply() { return QStringLiteral("/sys/class/power_supply/usb/"); }

DeviceProfile::ThermalPaths DeviceProfile::thermalPaths() const
{
    ThermalPaths paths;
    const QDir dir(QStringLiteral("/sys/class/thermal"));
    for (const QString &zone : dir.entryList({"thermal_zone*"}, QDir::Dirs)) {
        const QByteArray type = readDeviceFile(dir.filePath(zone + "/type"));
        const QString temperature = dir.filePath(zone + "/temp");
        if (tablet ? type.startsWith("cpu-0-") || type.startsWith("cpu-1-") : type == "cpu-1-max-step")
            paths.cpu.append(temperature);
        if (type == "skin-therm")
            paths.skin = temperature;
    }
    return paths;
}

QByteArray readDeviceFile(const QString &path)
{
    if (path.isEmpty())
        return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}

double maxTemperature(const QStringList &paths)
{
    double maxMilliC = 0;
    for (const QString &path : paths)
        maxMilliC = qMax(maxMilliC, readDeviceFile(path).toDouble());
    return maxMilliC / 1000.0;
}
