#include "system_tools.h"

#include <algorithm>
#include <QClipboard>
#include <QCollator>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimeZone>
#include <QUrl>

namespace {

const QString kVpn = QStringLiteral("/usr/local/bin/vpn");
constexpr int kTimeoutMs = 30000;
constexpr qsizetype kDialogDetailLimit = 320;
constexpr int kMaxClipboardItems = 15;
constexpr qsizetype kMaxClipboardChars = 4096;
Q_LOGGING_CATEGORY(lcSystemTools, "naomi.system.tools")

QString clipboardStorePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/naomi/desktop/clipboard.json");
}

bool isBuiltinNonKeyboard(const QString &name)
{
    static const QStringList ignored = {
        QStringLiteral("qpnp_pon"),
        QStringLiteral("gpio-keys"),
        QStringLiteral("xiaomi keyboard WakeUp"),
        QStringLiteral("xiaomi-touch"),
        QStringLiteral("qti-haptics"),
        QStringLiteral("NVTCapacitive"),
        QStringLiteral("kona-mtp"),
        QStringLiteral("naomi-virtual"),
    };
    for (const QString &prefix : ignored) {
        if (name.contains(prefix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool detectExternalKeyboard()
{
    QFile f(QStringLiteral("/proc/bus/input/devices"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString text = QString::fromUtf8(f.readAll());
    for (const QString &block : text.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts)) {
        QString name, handlers, bus;
        quint64 evBits = 0;
        for (const QString &line : block.split(u'\n')) {
            if (line.startsWith(QLatin1String("I:"))) {
                const int idx = line.indexOf(QLatin1String("Bus="));
                if (idx >= 0)
                    bus = line.mid(idx + 4, 4);
            } else if (line.startsWith(QLatin1String("N: Name="))) {
                name = line.mid(8).trimmed();
                if (name.startsWith(u'"') && name.endsWith(u'"') && name.size() >= 2)
                    name = name.mid(1, name.size() - 2);
            } else if (line.startsWith(QLatin1String("H: Handlers="))) {
                handlers = line.mid(12);
            } else if (line.startsWith(QLatin1String("B: EV="))) {
                evBits = line.mid(6).trimmed().toULongLong(nullptr, 16);
            }
        }
        if (name.isEmpty() || isBuiltinNonKeyboard(name))
            continue;
        const bool hasKbd = handlers.split(u' ', Qt::SkipEmptyParts).contains(QStringLiteral("kbd"));
        if (!hasKbd)
            continue;
        // Real USB/Bluetooth/pogo keyboards set EV_KEY (0x2) + EV_REP (0x100000) or come over USB (0003) / BT (0005).
        const bool hasRep = (evBits & 0x100000ULL) != 0;
        if (hasRep || bus == QLatin1String("0003") || bus == QLatin1String("0005"))
            return true;
    }
    return false;
}

bool detectExternalDisplay()
{
    const QDir drm(QStringLiteral("/sys/class/drm"));
    for (const QString &entry : drm.entryList({QStringLiteral("card*-DP-*"), QStringLiteral("card*-HDMI-*")}, QDir::Dirs)) {
        QFile f(drm.filePath(entry + QStringLiteral("/status")));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text) && f.readAll().trimmed() == "connected")
            return true;
    }
    return false;
}

bool executable(const QString &path)
{
    const QFileInfo file(path);
    return file.isFile() && file.isExecutable();
}

QString resolveDesktopIcon(const QString &icon)
{
    if (icon.isEmpty())
        return {};
    if (icon.startsWith(u'/'))
        return QFile::exists(icon) ? QUrl::fromLocalFile(icon).toString() : QString();
    static const QStringList dirs = {
        QStringLiteral("/usr/share/icons/hicolor/128x128/apps"),
        QStringLiteral("/usr/share/icons/hicolor/64x64/apps"),
        QStringLiteral("/usr/share/icons/hicolor/48x48/apps"),
        QStringLiteral("/usr/share/icons/hicolor/256x256/apps"),
        QStringLiteral("/usr/share/icons/hicolor/scalable/apps"),
        QStringLiteral("/usr/share/pixmaps"),
    };
    for (const QString &dir : dirs) {
        for (const char *ext : {".png", ".svg", ""}) {
            const QString cand = dir + u'/' + icon + QLatin1String(ext);
            if (QFile::exists(cand))
                return QUrl::fromLocalFile(cand).toString();
        }
    }
    return {};
}

QStringList parseExecCommand(const QString &execLine)
{
    const QStringList raw = QProcess::splitCommand(execLine);
    QStringList out;
    for (const QString &tok : raw) {
        if (tok.startsWith(u'%') && tok.size() == 2)
            continue;
        out.append(tok);
    }
    return out;
}

} // namespace

SystemTools::SystemTools(bool tablet)
    : m_vpnAvailable(tablet && executable(kVpn))
    , m_androidAvailable(tablet)
{
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        complete(status == QProcess::NormalExit && code == 0);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false);
    });
    if (tablet) {
        refreshLinuxApps();
        loadClipboardHistory();
        if (auto *cb = QGuiApplication::clipboard()) {
            connect(cb, &QClipboard::dataChanged, this, [this, cb] {
                if (m_ignoreClipboard)
                    return;
                recordClipboard(cb->text());
            });
            const QString cur = cb->text();
            if (!cur.isEmpty())
                recordClipboard(cur);
        }
        refreshHardwareState();
        m_hwTimer.setInterval(3000);
        connect(&m_hwTimer, &QTimer::timeout, this, &SystemTools::refreshHardwareState);
        m_hwTimer.start();
    }
}

void SystemTools::recordClipboard(const QString &text)
{
    const QString cleaned = text.left(kMaxClipboardChars);
    if (cleaned.trimmed().isEmpty())
        return;
    if (!m_clipboardHistory.isEmpty() && m_clipboardHistory.constFirst() == cleaned)
        return;
    m_clipboardHistory.removeAll(cleaned);
    m_clipboardHistory.prepend(cleaned);
    while (m_clipboardHistory.size() > kMaxClipboardItems)
        m_clipboardHistory.removeLast();
    saveClipboardHistory();
    emit clipboardChanged();
}

void SystemTools::loadClipboardHistory()
{
    QFile f(clipboardStorePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray())
        return;
    QStringList items;
    for (const QJsonValue &v : doc.array()) {
        const QString s = v.toString().left(kMaxClipboardChars);
        if (!s.trimmed().isEmpty() && !items.contains(s))
            items.append(s);
        if (items.size() >= kMaxClipboardItems)
            break;
    }
    m_clipboardHistory = items;
}

void SystemTools::saveClipboardHistory() const
{
    const QString path = clipboardStorePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray arr;
    for (const QString &s : m_clipboardHistory)
        arr.append(s);
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void SystemTools::copyClipboard(const QString &text)
{
    if (text.isEmpty())
        return;
    if (auto *cb = QGuiApplication::clipboard()) {
        m_ignoreClipboard = true;
        cb->setText(text);
        m_ignoreClipboard = false;
    }
    recordClipboard(text);
}

void SystemTools::removeClipboardAt(int index)
{
    if (index < 0 || index >= m_clipboardHistory.size())
        return;
    m_clipboardHistory.removeAt(index);
    saveClipboardHistory();
    emit clipboardChanged();
}

void SystemTools::clearClipboardHistory()
{
    if (m_clipboardHistory.isEmpty())
        return;
    m_clipboardHistory.clear();
    saveClipboardHistory();
    emit clipboardChanged();
}

bool SystemTools::readPcAgentConfig(QString &baseUrl, QString &token) const
{
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/pipa-bot");
    QString host = QStringLiteral("192.168.1.100");
    int port = 18765;
    QFile cfgFile(cfgDir + QStringLiteral("/config.json"));
    if (cfgFile.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(cfgFile.readAll()).object();
        const QJsonArray pcs = root.value(QStringLiteral("pcs")).toArray();
        if (!pcs.isEmpty()) {
            const QJsonObject pc = pcs.at(0).toObject();
            if (!pc.value(QStringLiteral("host")).toString().isEmpty())
                host = pc.value(QStringLiteral("host")).toString();
            if (pc.value(QStringLiteral("port")).toInt() > 0)
                port = pc.value(QStringLiteral("port")).toInt();
            if (!pc.value(QStringLiteral("token")).toString().isEmpty())
                token = pc.value(QStringLiteral("token")).toString();
        }
    }
    if (token.isEmpty()) {
        QFile envFile(cfgDir + QStringLiteral("/bot.env"));
        if (envFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            for (const QByteArray &raw : envFile.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.startsWith(QLatin1String("PC_AGENT_TOKEN="))) {
                    token = line.mid(15).trimmed();
                    break;
                }
            }
        }
    }
    baseUrl = QStringLiteral("http://%1:%2").arg(host).arg(port);
    return !token.isEmpty();
}

void SystemTools::pullPcClipboard()
{
    if (m_pcClipboardBusy)
        return;
    QString baseUrl, token;
    if (!readPcAgentConfig(baseUrl, token)) {
        m_pcClipboardStatus = QStringLiteral("Нет токена ПК-агента");
        emit clipboardChanged();
        return;
    }
    m_pcClipboardBusy = true;
    m_pcClipboardStatus = QStringLiteral("Забираю с ПК…");
    emit clipboardChanged();

    QNetworkRequest req(QUrl(baseUrl + QStringLiteral("/clipboard")));
    req.setRawHeader("X-Pipa-Token", token.toUtf8());
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_pcClipboardBusy = false;
        if (reply->error() != QNetworkReply::NoError) {
            m_pcClipboardStatus = QStringLiteral("ПК недоступен");
            emit clipboardChanged();
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString text = obj.value(QStringLiteral("text")).toString();
        if (text.trimmed().isEmpty()) {
            m_pcClipboardStatus = QStringLiteral("Буфер ПК пуст");
        } else {
            copyClipboard(text);
            m_pcClipboardStatus = QStringLiteral("Скопировано с ПК");
        }
        emit clipboardChanged();
    });
}

void SystemTools::pushPcClipboard(const QString &text)
{
    if (m_pcClipboardBusy)
        return;
    QString payload = text;
    if (payload.isEmpty()) {
        if (auto *cb = QGuiApplication::clipboard())
            payload = cb->text();
        if (payload.isEmpty() && !m_clipboardHistory.isEmpty())
            payload = m_clipboardHistory.constFirst();
    }
    if (payload.isEmpty()) {
        m_pcClipboardStatus = QStringLiteral("Буфер планшета пуст");
        emit clipboardChanged();
        return;
    }
    QString baseUrl, token;
    if (!readPcAgentConfig(baseUrl, token)) {
        m_pcClipboardStatus = QStringLiteral("Нет токена ПК-агента");
        emit clipboardChanged();
        return;
    }
    m_pcClipboardBusy = true;
    m_pcClipboardStatus = QStringLiteral("Отправляю на ПК…");
    emit clipboardChanged();

    QNetworkRequest req(QUrl(baseUrl + QStringLiteral("/clipboard")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-Pipa-Token", token.toUtf8());
    req.setTransferTimeout(4000);
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("text"), payload}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_net.post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_pcClipboardBusy = false;
        m_pcClipboardStatus = reply->error() == QNetworkReply::NoError
            ? QStringLiteral("Отправлено на ПК")
            : QStringLiteral("ПК недоступен");
        emit clipboardChanged();
    });
}

void SystemTools::refreshHardwareState()
{
    const bool kb = detectExternalKeyboard();
    const bool dp = detectExternalDisplay();
    if (kb == m_hardwareKeyboard && dp == m_externalDisplay)
        return;
    m_hardwareKeyboard = kb;
    m_externalDisplay = dp;
    emit hardwareChanged();
}

namespace {

QString readTrimmedFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

bool queryServiceActive(const QString &unit, bool userUnit)
{
    QProcess p;
    QStringList args;
    if (userUnit)
        args << QStringLiteral("--user");
    args << QStringLiteral("is-active") << unit;
    p.start(QStringLiteral("systemctl"), args);
    if (!p.waitForFinished(1200))
        return false;
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed() == QLatin1String("active");
}

QString readLocalIp()
{
    QProcess p;
    p.start(QStringLiteral("hostname"), {QStringLiteral("-I")});
    if (!p.waitForFinished(1000))
        return {};
    const QStringList ips = QString::fromUtf8(p.readAllStandardOutput()).trimmed().split(u' ', Qt::SkipEmptyParts);
    for (const QString &ip : ips) {
        if (ip.startsWith(QLatin1String("192.168.")) || ip.startsWith(QLatin1String("10.")))
            return ip;
    }
    return ips.isEmpty() ? QString() : ips.constFirst();
}

} // namespace

void SystemTools::refreshSystemInfo()
{
    m_powerProfile = readTrimmedFile(QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"));
    if (m_powerProfile.isEmpty())
        m_powerProfile = QStringLiteral("schedutil");

    // Temperatures (CPU prime cluster + battery)
    double cpuC = 0.0, batC = 0.0;
    const QDir thermal(QStringLiteral("/sys/class/thermal"));
    for (const QString &z : thermal.entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs)) {
        const QString type = readTrimmedFile(thermal.filePath(z + QStringLiteral("/type")));
        if (type == QLatin1String("cpu-1-7-usr") || type == QLatin1String("cpu-1-3-usr")) {
            const double t = readTrimmedFile(thermal.filePath(z + QStringLiteral("/temp"))).toDouble() / 1000.0;
            if (t > cpuC)
                cpuC = t;
        } else if (type == QLatin1String("battery")) {
            batC = readTrimmedFile(thermal.filePath(z + QStringLiteral("/temp"))).toDouble() / 1000.0;
        }
    }

    // ZRAM usage
    qint64 zramUsedMb = 0, zramTotalMb = 0;
    const QString mmStat = readTrimmedFile(QStringLiteral("/sys/block/zram0/mm_stat"));
    if (!mmStat.isEmpty()) {
        const QStringList parts = mmStat.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!parts.isEmpty())
            zramUsedMb = parts.at(0).toLongLong() / (1024 * 1024);
    }
    const qint64 diskSz = readTrimmedFile(QStringLiteral("/sys/block/zram0/disksize")).toLongLong();
    if (diskSz > 0)
        zramTotalMb = diskSz / (1024 * 1024);

    // Storage
    const QStorageInfo dataStore(QStringLiteral("/userdata"));
    const QStorageInfo rootStore(QStringLiteral("/"));
    const QStorageInfo &store = dataStore.isValid() && dataStore.bytesTotal() > 0 ? dataStore : rootStore;
    const double freeGb = store.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
    const double totalGb = store.bytesTotal() / (1024.0 * 1024.0 * 1024.0);

    QString tz = readTrimmedFile(QStringLiteral("/etc/timezone"));
    if (tz.isEmpty())
        tz = QString::fromUtf8(QTimeZone::systemTimeZoneId());

    m_sysInfo = QVariantMap{
        {QStringLiteral("cpuTemp"), QString::number(cpuC, 'f', 1)},
        {QStringLiteral("batTemp"), QString::number(batC, 'f', 1)},
        {QStringLiteral("zramUsedMb"), zramUsedMb},
        {QStringLiteral("zramTotalMb"), zramTotalMb},
        {QStringLiteral("diskFreeGb"), QString::number(freeGb, 'f', 1)},
        {QStringLiteral("diskTotalGb"), QString::number(totalGb, 'f', 1)},
        {QStringLiteral("timezone"), tz},
        {QStringLiteral("ip"), readLocalIp()},
        {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
    };

    m_services = QVariantList{
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("tg-bot")},
            {QStringLiteral("name"), QStringLiteral("Telegram-бот")},
            {QStringLiteral("desc"), QStringLiteral("@your_pipa_bot (управление планшетом, ПК и ТВ)")},
            {QStringLiteral("active"), queryServiceActive(QStringLiteral("pipa-tg-bot.service"), true)},
            {QStringLiteral("controllable"), true},
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("camera")},
            {QStringLiteral("name"), QStringLiteral("Служба камер")},
            {QStringLiteral("desc"), QStringLiteral("Доступ к задней (13 МП) и фронтальной (8 МП) камерам")},
            {QStringLiteral("active"), queryServiceActive(QStringLiteral("cameraservice-trust-stored.service"), true)},
            {QStringLiteral("controllable"), true},
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("waydroid")},
            {QStringLiteral("name"), QStringLiteral("Waydroid (Android)")},
            {QStringLiteral("desc"), QStringLiteral("Контейнер Android-приложений (автостоп через 10 мин простоя)")},
            {QStringLiteral("active"), queryServiceActive(QStringLiteral("waydroid-container.service"), false)},
            {QStringLiteral("controllable"), true},
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("desktop-commander")},
            {QStringLiteral("name"), QStringLiteral("Desktop Commander")},
            {QStringLiteral("desc"), QStringLiteral("Внешний удалённый доступ (выключен по умолчанию)")},
            {QStringLiteral("active"), queryServiceActive(QStringLiteral("desktop-commander.service"), true)},
            {QStringLiteral("controllable"), true},
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("ssh")},
            {QStringLiteral("name"), QStringLiteral("SSH-сервер")},
            {QStringLiteral("desc"), QStringLiteral("Удалённый вход с ПК по Wi-Fi")},
            {QStringLiteral("active"), queryServiceActive(QStringLiteral("ssh.service"), false)},
            {QStringLiteral("controllable"), false},
        },
    };
    emit systemChanged();
}

void SystemTools::runSystemCommand(const QString &program, const QStringList &arguments,
                                   const QString &okMsg, const QString &errMsg)
{
    if (m_sysBusy)
        return;
    m_sysBusy = true;
    m_systemStatus = QStringLiteral("Выполняю…");
    emit systemChanged();

    auto *proc = new QProcess(this);
    proc->setStandardInputFile(QProcess::nullDevice());
    connect(proc, &QProcess::finished, this, [this, proc, okMsg, errMsg](int code, QProcess::ExitStatus st) {
        proc->deleteLater();
        m_sysBusy = false;
        m_systemStatus = (st == QProcess::NormalExit && code == 0) ? okMsg : errMsg;
        refreshSystemInfo();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, errMsg](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            proc->deleteLater();
            m_sysBusy = false;
            m_systemStatus = errMsg;
            emit systemChanged();
        }
    });
    proc->start(program, arguments);
}

void SystemTools::setPowerProfile(const QString &profile)
{
    static const QStringList allowed = {
        QStringLiteral("powersave"),
        QStringLiteral("schedutil"),
        QStringLiteral("performance"),
    };
    if (!allowed.contains(profile))
        return;
    const QString cmd = QStringLiteral(
        "for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do "
        "[ -w \"$p\" ] && echo %1 > \"$p\"; done").arg(profile);
    const QString label = profile == QLatin1String("powersave") ? QStringLiteral("Эконом (powersave)")
                        : profile == QLatin1String("performance") ? QStringLiteral("Максимум (performance)")
                                                                  : QStringLiteral("Баланс (schedutil)");
    runSystemCommand(QStringLiteral("/usr/bin/sudo"), {QStringLiteral("-n"), QStringLiteral("/bin/sh"), QStringLiteral("-c"), cmd},
                     QStringLiteral("Профиль CPU: %1").arg(label),
                     QStringLiteral("Не удалось переключить профиль CPU"));
}

void SystemTools::toggleService(const QString &id, bool enable)
{
    struct SvcDef { const char *id; const char *unit; bool user; const char *label; };
    static const SvcDef table[] = {
        {"tg-bot", "pipa-tg-bot.service", true, "Telegram-бот"},
        {"camera", "cameraservice-trust-stored.service", true, "Служба камер"},
        {"waydroid", "waydroid-container.service", false, "Waydroid"},
        {"desktop-commander", "desktop-commander.service", true, "Desktop Commander"},
    };
    for (const auto &s : table) {
        if (id != QLatin1String(s.id))
            continue;
        const QString action = enable ? QStringLiteral("start") : QStringLiteral("stop");
        const QString okMsg = QStringLiteral("%1: %2").arg(QString::fromUtf8(s.label),
                                                           enable ? QStringLiteral("запущено") : QStringLiteral("остановлено"));
        const QString errMsg = QStringLiteral("Ошибка управления: %1").arg(QString::fromUtf8(s.label));
        if (s.user) {
            runSystemCommand(QStringLiteral("systemctl"), {QStringLiteral("--user"), action, QLatin1String(s.unit)}, okMsg, errMsg);
        } else {
            runSystemCommand(QStringLiteral("/usr/bin/sudo"), {QStringLiteral("-n"), QStringLiteral("systemctl"), action, QLatin1String(s.unit)}, okMsg, errMsg);
        }
        return;
    }
}

void SystemTools::cleanJournal()
{
    runSystemCommand(QStringLiteral("/usr/bin/sudo"),
                     {QStringLiteral("-n"), QStringLiteral("journalctl"), QStringLiteral("--vacuum-size=20M")},
                     QStringLiteral("Журнал системы очищен (лимит 20 МБ)"),
                     QStringLiteral("Не удалось очистить журнал"));
}

void SystemTools::setTimezone(const QString &tz)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_/+.-]{2,64}$"));
    if (!re.match(tz).hasMatch() || tz.contains(QLatin1String("..")))
        return;
    runSystemCommand(QStringLiteral("/usr/bin/sudo"),
                     {QStringLiteral("-n"), QStringLiteral("timedatectl"), QStringLiteral("set-timezone"), tz},
                     QStringLiteral("Часовой пояс: %1").arg(tz),
                     QStringLiteral("Не удалось сменить часовой пояс"));
}

void SystemTools::syncTime()
{
    runSystemCommand(QStringLiteral("/usr/bin/sudo"),
                     {QStringLiteral("-n"), QStringLiteral("systemctl"), QStringLiteral("restart"), QStringLiteral("systemd-timesyncd.service")},
                     QStringLiteral("Время синхронизировано по NTP"),
                     QStringLiteral("Не удалось синхронизировать время"));
}

void SystemTools::refreshLinuxApps()
{
    const QStringList dirs = {
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QStringLiteral("/usr/share/applications"),
    };
    QSet<QString> seenIds;
    QVariantList apps;
    for (const QString &dirPath : dirs) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;
        for (const QString &file : dir.entryList({QStringLiteral("*.desktop")}, QDir::Files)) {
            if (file.startsWith(QLatin1String("waydroid.")) || file.contains(QLatin1String(".ubports_"))
                || file.startsWith(QLatin1String("org.popkaphone.")) || file.startsWith(QLatin1String("userapp-Telegram"))
                || file == QLatin1String("Waydroid.desktop"))
                continue;
            const QString id = file.left(file.size() - 8);
            if (seenIds.contains(id))
                continue;
            QFile f(dir.filePath(file));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            QString type, name, nameRu, execLine, icon;
            bool noDisplay = false, hidden = false, terminal = false, inEntry = false;
            for (const QByteArray &raw : f.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.startsWith(u'[')) {
                    inEntry = line == QLatin1String("[Desktop Entry]");
                    continue;
                }
                if (!inEntry || line.isEmpty() || line.startsWith(u'#'))
                    continue;
                if (line.startsWith(QLatin1String("Type=")))
                    type = line.mid(5);
                else if (line.startsWith(QLatin1String("Name[ru]=")))
                    nameRu = line.mid(9);
                else if (line.startsWith(QLatin1String("Name=")))
                    name = line.mid(5);
                else if (line.startsWith(QLatin1String("Exec=")))
                    execLine = line.mid(5);
                else if (line.startsWith(QLatin1String("Icon=")))
                    icon = line.mid(5);
                else if (line == QLatin1String("NoDisplay=true"))
                    noDisplay = true;
                else if (line == QLatin1String("Hidden=true"))
                    hidden = true;
                else if (line == QLatin1String("Terminal=true"))
                    terminal = true;
            }
            if (type != QLatin1String("Application") || noDisplay || hidden || execLine.isEmpty())
                continue;
            const QStringList parts = parseExecCommand(execLine);
            if (parts.isEmpty())
                continue;
            const QString prog = parts.constFirst();
            const QString resolvedProg = prog.startsWith(u'/') ? prog : QStandardPaths::findExecutable(prog);
            if (resolvedProg.isEmpty())
                continue;
            const QString displayName = !nameRu.isEmpty() ? nameRu : name;
            if (displayName.isEmpty())
                continue;
            seenIds.insert(id);
            apps.append(QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("name"), displayName},
                {QStringLiteral("program"), resolvedProg},
                {QStringLiteral("args"), parts.mid(1)},
                {QStringLiteral("icon"), resolveDesktopIcon(icon)},
                {QStringLiteral("terminal"), terminal},
                {QStringLiteral("label"), terminal ? QStringLiteral("CLI") : QStringLiteral("LINUX")},
            });
        }
    }
    QCollator collator(QLocale(QStringLiteral("ru_RU")));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(apps.begin(), apps.end(), [&collator](const QVariant &a, const QVariant &b) {
        return collator.compare(a.toMap().value(QStringLiteral("name")).toString(),
                                b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    if (apps == m_linuxApps)
        return;
    m_linuxApps = apps;
    emit linuxAppsChanged();
}

void SystemTools::refreshVpn()
{
    if (!m_vpnAvailable || busy())
        return;
    m_vpnError.clear();
    run(Operation::List, kVpn, {QStringLiteral("list")});
}

void SystemTools::setVpn(const QString &profile)
{
    if (!m_vpnAvailable || busy() || (!profile.isEmpty() && !m_profiles.contains(profile)))
        return;
    m_vpnError.clear();
    const QStringList args = profile.isEmpty() ? QStringList{"-n", kVpn, "off"} : QStringList{"-n", kVpn, "on", profile};
    run(Operation::VpnChange, QStringLiteral("/usr/bin/sudo"), args);
}

void SystemTools::bootAndroid()
{
    if (!m_androidAvailable || busy())
        return;
    m_androidError.clear();
    m_operation = Operation::Reboot;
    emit changed();
    const auto call = QDBusMessage::createMethodCall("org.naomi.Phone", "/org/naomi/Phone/System",
                                                      "org.naomi.Phone.System", "RebootOneUi");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QDBusError error = watcher->error();
        if (!error.isValid())
            return;   // перезагрузка принята, подтверждение остаётся закрытым для повторного ввода

        m_androidError = error.type() == QDBusError::NoReply
            ? QStringLiteral("Служба перезагрузки не ответила. Смена слота ещё может выполняться.")
            : QStringLiteral("Не удалось перезагрузиться в One UI.");
        const QString detail = error.message().simplified();
        if (!detail.isEmpty()) {
            m_androidError += u'\n' + detail.left(kDialogDetailLimit);
            if (detail.size() > kDialogDetailLimit)
                m_androidError += u'…';
        }
        qCWarning(lcSystemTools, "переход в One UI: %s", qPrintable(detail.left(4096)));
        m_operation = Operation::Idle;
        emit changed();
    });
}

void SystemTools::run(Operation op, const QString &program, const QStringList &arguments)
{
    m_operation = op;
    emit changed();
    m_timeout.start(kTimeoutMs);
    m_process.start(program, arguments);
}

void SystemTools::complete(bool ok)
{
    m_timeout.stop();
    const QString output = QString::fromUtf8(m_process.readAllStandardOutput()).trimmed();
    if (m_operation == Operation::List) {
        // vpn list: одно имя в строке, допускаем отступ и маркер списка.
        static const QRegularExpression name(QStringLiteral(R"(^\s*(?:[-*]\s+)?([a-zA-Z0-9][a-zA-Z0-9_.-]{0,63})\s*$)"));
        m_profiles.clear();
        if (ok) {
            for (const QString &line : output.split(u'\n')) {
                const auto match = name.match(line);
                if (match.hasMatch())
                    m_profiles.append(match.captured(1));
            }
            m_profiles.removeDuplicates();
        } else {
            m_vpnError = QStringLiteral("Не удалось получить список VPN");
        }
        run(Operation::Status, kVpn, {QStringLiteral("status")});
        return;
    }
    if (m_operation == Operation::VpnChange) {
        if (!ok)
            m_vpnError = QStringLiteral("Не удалось переключить VPN. Проверьте права sudo и выбранный профиль.");
        run(Operation::Status, kVpn, {QStringLiteral("status")});
        return;
    }
    if (m_operation == Operation::Status) {
        m_status = output.left(2048);
        if (m_status.isEmpty() && !ok && m_vpnError.isEmpty())
            m_vpnError = QStringLiteral("Не удалось проверить состояние VPN");
    }
    m_operation = Operation::Idle;
    emit changed();
}
