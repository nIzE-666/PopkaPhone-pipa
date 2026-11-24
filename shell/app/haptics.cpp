#include "haptics.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QSettings>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

Q_LOGGING_CATEGORY(lcHaptics, "naomi.haptics")

namespace {
QString settingsPath() { return QDir::homePath() + "/.config/naomi/haptics.conf"; }

// устройство ввода с вибромотором: по имени (qti-haptics) и с FF_CONSTANT
QString findDevice()
{
    for (const QString &ev : QDir("/sys/class/input").entryList({"event*"})) {
        QFile name("/sys/class/input/" + ev + "/device/name");
        if (name.open(QIODevice::ReadOnly) && name.readAll().trimmed().contains("haptics"))
            return "/dev/input/" + ev;
    }
    return {};
}
} // namespace

Haptics::Haptics(QObject *parent) : QObject(parent)
{
    m_enabled = QSettings(settingsPath(), QSettings::IniFormat).value("keyboard", true).toBool();
    const QString dev = findDevice();
    if (dev.isEmpty())
        return;
    m_fd = ::open(QFile::encodeName(dev).constData(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (m_fd < 0)
        qCWarning(lcHaptics, "вибромотор %s не открыть: %s", qPrintable(dev), strerror(errno));
    else
        qCInfo(lcHaptics, "вибромотор: %s", qPrintable(dev));
}

Haptics::~Haptics()
{
    if (m_fd < 0)
        return;
    for (const int id : std::as_const(m_effects))
        ioctl(m_fd, EVIOCRMFF, id);
    ::close(m_fd);
}

void Haptics::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    QSettings(settingsPath(), QSettings::IniFormat).setValue("keyboard", on);
    emit enabledChanged();
}

int Haptics::effectFor(int ms)
{
    const auto it = m_effects.constFind(ms);
    if (it != m_effects.constEnd())
        return *it;
    ff_effect e;
    std::memset(&e, 0, sizeof e);
    e.type = FF_CONSTANT;
    e.id = -1;
    e.replay.length = ms;
    e.u.constant.level = 0x7fff;
    if (ioctl(m_fd, EVIOCSFF, &e) < 0) {
        qCWarning(lcHaptics, "эффект %d мс не загрузить: %s", ms, strerror(errno));
        return -1;
    }
    m_effects.insert(ms, e.id);
    return e.id;
}

void Haptics::tap(int ms)
{
    if (m_fd < 0 || !m_enabled)
        return;
    const int id = effectFor(std::clamp(ms, 5, 200));
    if (id < 0)
        return;
    input_event play;
    std::memset(&play, 0, sizeof play);
    play.type = EV_FF;
    play.code = id;
    play.value = 1;
    if (::write(m_fd, &play, sizeof play) != sizeof play)
        qCDebug(lcHaptics, "импульс не ушёл: %s", strerror(errno));
}
