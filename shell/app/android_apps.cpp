#include "android_apps.h"

#include <QCollator>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QLoggingCategory>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QWaylandCompositor>
#include <QWaylandOutput>
#include <QWaylandQuickItem>
#include <QWaylandSeat>
#include <QtWaylandCompositor/private/qwaylandoutput_p.h>

#include <wayland-server-core.h>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_LOGGING_CATEGORY(lcAndroid, "naomi.android")

namespace {
const QString kWaydroid = QStringLiteral("/usr/bin/waydroid");
const QString kSessionName = QStringLiteral("id.waydro.Session");
const QString kPhoneService = QStringLiteral("org.naomi.Phone");          // naomid: задачи Android (android.h там)
const QString kAndroidPath = QStringLiteral("/org/naomi/Phone/Android");
const QString kAndroidIface = QStringLiteral("org.naomi.Phone.Android");

// Не в списке «03 ANDROID»: системные приложения Android (как system_apps у Waydroid) и Google-приложения образа.
// Google Play и «Настройки» Android — кнопками на шильдике списка.
const QSet<QString> kHidden = {
    QStringLiteral("com.android.calculator2"), QStringLiteral("com.android.camera2"),
    QStringLiteral("com.android.contacts"), QStringLiteral("com.android.deskclock"),
    QStringLiteral("com.android.documentsui"), QStringLiteral("com.android.email"),
    QStringLiteral("com.android.gallery3d"), QStringLiteral("com.android.inputmethod.latin"),
    QStringLiteral("com.android.settings"), QStringLiteral("com.android.vending"),
    QStringLiteral("com.google.android.gms"), QStringLiteral("com.google.android.apps.messaging"),
    QStringLiteral("com.google.android.apps.restore"), QStringLiteral("com.google.android.contacts"),
    QStringLiteral("com.google.android.dialer"), QStringLiteral("org.lineageos.aperture"),
    QStringLiteral("org.lineageos.eleven"), QStringLiteral("org.lineageos.etar"),
    QStringLiteral("org.lineageos.jelly"), QStringLiteral("org.lineageos.recorder"),
};
}

AndroidApps::AndroidApps(const QString &waylandSocket, QObject *parent)
    : QObject(parent)
    , m_socket(waylandSocket)
    , m_installed(QFile::exists(kWaydroid) && QFile::exists(QStringLiteral("/var/lib/waydroid/waydroid.cfg")))
    , m_appsDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications")
{
    connect(&m_scroll, &AndroidScroll::released, this, [this](QWaylandQuickItem *layer) { emit scrollEnded(layer); });
    if (!m_installed) {
        qCInfo(lcAndroid, "Waydroid не настроен — Android-приложений нет (docs/ANDROID.md)");
        return;
    }
    // Waydroid переписывает файлы приложений пачкой (каждый — через временный и переименование): читаем, когда
    // папка успокоилась
    m_reread.setSingleShot(true);
    m_reread.setInterval(300);
    connect(&m_reread, &QTimer::timeout, this, &AndroidApps::readApps);
    // Обычно Android готов через 12–25 с после включения; 180 с — встал (служба вывода Waydroid уснула, не отметившись:
    // SurfaceFlinger ждёт её минутами, docs/ANDROID.md, «Подводные камни») — перезапуск
    m_startTimeout.setSingleShot(true);
    m_startTimeout.setInterval(180000);
    connect(&m_startTimeout, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting)
            restart("не загрузился за 180 с", m_pending);
    });
    m_sleepTimer.setSingleShot(true);
    connect(&m_sleepTimer, &QTimer::timeout, this, [this] { sleep(true); });
    m_idleStopTimer.setSingleShot(true);
    connect(&m_idleStopTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::On && !m_windows && m_pending.isEmpty()) {
            qCInfo(lcAndroid, "Android без окон в простое — останавливаю сессию для экономии батареи");
            setState(State::Stopping);
            stop();
        }
    });
    connect(&m_dirWatch, &QFileSystemWatcher::directoryChanged, this, [this] { m_reread.start(); });
    QDir().mkpath(m_appsDir);
    m_dirWatch.addPath(m_appsDir);
    readApps();

    // задачи Android (где окна) — от naomid, всегда, пока оболочка жива: стол смотрят и с погашенным экраном телефона,
    // а зеркала naomiPhone тогда шину не слушают (phone_client.h)
    QDBusConnection::systemBus().connect(kPhoneService, kAndroidPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                         QStringLiteral("PropertiesChanged"), this,
                                         SLOT(onTaskProps(QString,QVariantMap,QStringList)));

    m_watch = new QDBusServiceWatcher(kSessionName, QDBusConnection::sessionBus(),
                                      QDBusServiceWatcher::WatchForRegistration
                                          | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_watch, &QDBusServiceWatcher::serviceRegistered, this, &AndroidApps::sessionAppeared);
    connect(m_watch, &QDBusServiceWatcher::serviceUnregistered, this, &AndroidApps::sessionGone);
    // сессия от прошлой оболочки: её контейнер держит старый сокет Wayland — окна не придут; выключить
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered(kSessionName)) {
        qCInfo(lcAndroid, "сессия Android от прошлой оболочки — выключаю");
        setState(State::Stopping);
        stop();
    }
}

QString AndroidApps::state() const
{
    switch (m_state) {
    case State::Off: return QStringLiteral("off");
    case State::Starting: return QStringLiteral("starting");
    case State::On: return QStringLiteral("on");
    case State::Stopping: return QStringLiteral("stopping");
    }
    return {};
}

void AndroidApps::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    qCInfo(lcAndroid, "Android: %s", qPrintable(state()));
    emit stateChanged();
}

bool AndroidApps::isAndroid(qint64 pid)
{
    if (!m_installed || pid <= 0)
        return false;
    const auto it = m_clients.constFind(pid);
    if (it != m_clients.cend())
        return *it;
    QFile f(QStringLiteral("/proc/%1/cgroup").arg(pid));
    const bool android = f.open(QIODevice::ReadOnly) && f.readAll().contains("lxc.payload.waydroid");
    m_clients.insert(pid, android);
    return android;
}

void AndroidApps::launch(const QString &pkg)
{
    if (!m_installed || pkg.isEmpty())
        return;
    m_idleStopTimer.stop();
    emit launching(pkg);
    m_launchedAt.insert(pkg, QDateTime::currentMSecsSinceEpoch());
    switch (m_state) {
    case State::On:
        if (m_frozen) {   // открыть приложение — оттаять (waydroid app launch оттаивает и сам — состояние вслед)
            m_wantFrozen = false;
            applyFreeze();
        }
        runWaydroid({QStringLiteral("app"), QStringLiteral("launch"), pkg});
        break;
    case State::Off:
        if (!m_pending.contains(pkg))
            m_pending << pkg;
        startSession();
        break;
    case State::Starting:
    case State::Stopping:   // включится снова, когда старая сессия уйдёт (sessionGone)
        if (!m_pending.contains(pkg))
            m_pending << pkg;
        break;
    }
}

// Сессия — дочерний процесс оболочки (в её группе процессов): служба оболочки остановилась или упала — systemd
// останавливает и сессию, а та — контейнер. WAYLAND_DISPLAY — сокет оболочки: его контейнер привязывает к себе.
void AndroidApps::startSession()
{
    if (m_session)
        return;
    setState(State::Starting);
    m_sound.start();   // «приёмная» звука — до службы звука Android (она откроет поток с первым звуком)
    m_session = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WAYLAND_DISPLAY"), m_socket);
    m_session->setProcessEnvironment(env);
    // Журнал сессии — в журнал оболочки, построчно; «Android with user 0 is ready» (Waydroid: пользователь Android
    // разблокирован, система загружена) — Android готов: раньше команда «открыть приложение» доходила до Android, ещё
    // не готового его принять, и терялась (проверено 18 ноября 2026: Google Play после включения не открывался)
    m_session->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_session, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_session->canReadLine()) {
            const QString line = QString::fromUtf8(m_session->readLine()).trimmed();
            if (line.isEmpty())
                continue;
            qCInfo(lcAndroid, "waydroid: %s", qPrintable(line));
            if (line.contains(QLatin1String("is ready")))
                androidReady();
        }
    });
    connect(m_session, &QProcess::finished, this, [this](int code) {
        qCInfo(lcAndroid, "сессия Android завершилась, код %d", code);
        m_session->deleteLater();
        m_session = nullptr;
        if (m_state == State::Starting)   // не поднялась (имени на шине так и не было)
            sessionGone();
        else if (m_state == State::Off && !m_pending.isEmpty())   // имя ушло раньше процесса, а приложение ждут
            startSession();
        else if (m_state == State::Stopping)   // перезапуск (restart): имя уйдёт следом; не было его — ушла и так
            QTimer::singleShot(2000, this, [this] {
                if (m_state == State::Stopping && !m_session
                    && !QDBusConnection::sessionBus().interface()->isServiceRegistered(kSessionName))
                    sessionGone();
            });
    });
    m_session->start(kWaydroid, {QStringLiteral("session"), QStringLiteral("start")});
    m_startTimeout.start();
    qCInfo(lcAndroid, "включаю Android: waydroid session start");
}

void AndroidApps::runWaydroid(const QStringList &args)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WAYLAND_DISPLAY"), m_socket);
    p.setProcessEnvironment(env);
    p.setProgram(kWaydroid);
    p.setArguments(args);
    p.setStandardOutputFile(QProcess::nullDevice());
    if (!p.startDetached())
        qCWarning(lcAndroid, "waydroid %s: не запустился", qPrintable(args.join(' ')));
}

// Сессия на шине: своя — ждём готовности Android (androidReady), чужая (запущена не оболочкой) — считаем готовой
void AndroidApps::sessionAppeared()
{
    if (m_state == State::Stopping || m_session)
        return;
    m_sound.start();
    androidReady();
}

void AndroidApps::androidReady()
{
    m_startTimeout.stop();
    setState(State::On);
    refreshTasks();
    applyFreeze();
    sendSleep();
    const QStringList pending = std::exchange(m_pending, {});
    for (const QString &pkg : pending)
        runWaydroid({QStringLiteral("app"), QStringLiteral("launch"), pkg});
    updateIdleStop();
}

void AndroidApps::sessionGone()
{
    m_startTimeout.stop();
    m_idleStopTimer.stop();
    m_clients.clear();   // pid контейнера больше не наши
    if (m_frozen) {
        m_frozen = false;
        emit frozenChanged();
    }
    m_sound.stop();
    m_sleepTimer.stop();
    m_sleepSent = false;   // новый Android включается бодрым
    if (m_asleep) {
        m_asleep = false;
        emit asleepChanged();
    }
    setState(State::Off);
    if (!m_pending.isEmpty())   // пока старая уходила, просили приложение — включить снова
        startSession();
}

void AndroidApps::setCompositor(QWaylandCompositor *compositor)
{
    m_compositor = compositor;
    m_scroll.setCompositor(compositor);
    if (compositor)
        wl_display_set_global_filter(compositor->display(), &AndroidApps::filterGlobal, this);
}

void AndroidApps::setDeskOutput(QWaylandOutput *output)
{
    // данные глобального объекта wl_output у Qt — его QtWaylandServer::wl_output (QWaylandOutputPrivate)
    m_deskOutput = output ? static_cast<QtWaylandServer::wl_output *>(QWaylandOutputPrivate::get(output)) : nullptr;
}

void AndroidApps::setWinOutput(QWaylandOutput *output, std::function<bool(qint64)> isWin)
{
    m_winOutput = output ? static_cast<QtWaylandServer::wl_output *>(QWaylandOutputPrivate::get(output)) : nullptr;
    m_winClient = std::move(isWin);
}

bool AndroidApps::filterGlobal(const wl_client *client, const wl_global *global, void *data)
{
    const char *name = wl_global_get_interface(global)->name;
    const bool output = !std::strcmp(name, "wl_output");
    const bool presentation = !std::strcmp(name, "wp_presentation");
    if (!output && !presentation)
        return true;
    // NAOMI_ANDROID_VSYNC=0 — как было до 18 ноября 2026 (замер «было/стало»): Android видит все выводы, отметок нет
    static const bool vsync = qEnvironmentVariable("NAOMI_ANDROID_VSYNC") != QLatin1String("0");
    if (!vsync)
        return output;
    auto *self = static_cast<AndroidApps *>(data);
    pid_t pid = 0;
    wl_client_get_credentials(const_cast<wl_client *>(client), &pid, nullptr, nullptr);
    const bool android = self->isAndroid(pid);
    if (presentation)
        return android || !self->m_installed;   // без Android фильтра раньше не было вовсе — отметки видели все
    const void *which = wl_global_get_user_data(global);
    if (self->m_winOutput && self->m_winClient) {
        const bool win = self->m_winClient(pid);
        if (win || which == self->m_winOutput)
            return win && which == self->m_winOutput;
    }
    return !android || !self->m_deskOutput || which == self->m_deskOutput;
}

void AndroidApps::wheel(QObject *layer, QPointF at, QPointF d, bool end)
{
    m_scroll.scroll(qobject_cast<QWaylandQuickItem *>(layer), at, d, end);
}

void AndroidApps::refocus(QObject *item)
{
    auto *window = qobject_cast<QWaylandQuickItem *>(item);
    QWaylandSeat *seat = m_compositor ? m_compositor->defaultSeat() : nullptr;
    if (!window || !window->surface() || !seat)
        return;
    seat->setKeyboardFocus(nullptr);
    window->takeFocus();
}

void AndroidApps::refreshTasks()
{
    QDBusMessage m = QDBusMessage::createMethodCall(kPhoneService, kAndroidPath, kAndroidIface,
                                                    QStringLiteral("RefreshTasks"));
    QDBusConnection::systemBus().asyncCall(m);
}

void AndroidApps::onTaskProps(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (iface == kAndroidIface && changed.contains(QStringLiteral("Tasks")))
        applyTasks(changed.value(QStringLiteral("Tasks")).toString());
}

void AndroidApps::applyTasks(const QString &json)
{
    const QVariantList tasks = QJsonDocument::fromJson(json.toUtf8()).array().toVariantList();
    if (tasks == m_tasks)
        return;
    m_tasks = tasks;
    emit tasksChanged();
}

void AndroidApps::moveTask(const QString &pkg, QRect from, QPoint to)
{
    if (pkg.isEmpty() || from.isEmpty() || from.topLeft() == to)
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kPhoneService, kAndroidPath, kAndroidIface, QStringLiteral("MoveTask"));
    m << pkg << from.x() << from.y() << from.width() << from.height() << to.x() << to.y();
    QDBusConnection::systemBus().asyncCall(m);
}

void AndroidApps::resizeTask(const QString &pkg, QRect from, QRect to)
{
    if (pkg.isEmpty() || from.isEmpty() || to.isEmpty() || from == to)
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kPhoneService, kAndroidPath, kAndroidIface,
                                                    QStringLiteral("ResizeTask"));
    m << pkg << from.x() << from.y() << from.width() << from.height() << to.x() << to.y() << to.width() << to.height();
    QDBusConnection::systemBus().asyncCall(m);
}

void AndroidApps::freeze(bool on)
{
    m_wantFrozen = on;
    applyFreeze();
}

void AndroidApps::applyFreeze()
{
    if (m_state != State::On || m_wantFrozen == m_frozen)
        return;
    m_frozen = m_wantFrozen;
    emit frozenChanged();
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("id.waydro.Container"), QStringLiteral("/ContainerManager"),
                                                    QStringLiteral("id.waydro.ContainerManager"),
                                                    m_frozen ? QStringLiteral("Freeze") : QStringLiteral("Unfreeze"));
    QDBusConnection::systemBus().asyncCall(m, 20000);
    qCInfo(lcAndroid, "Android %s", m_frozen ? "заморожен (стол не смотрят)" : "оттаял");
    if (!m_frozen)
        sendSleep();   // что решили про сон, пока он стоял
}

void AndroidApps::setSeen(bool seen, int grace)
{
    if (seen) {
        m_sleepTimer.stop();
        sleep(false);
    } else if (!m_asleep && (!m_sleepTimer.isActive() || m_sleepTimer.remainingTime() > grace)) {
        m_sleepTimer.start(std::max(0, grace));
    }
}

void AndroidApps::setWindows(bool any)
{
    if (any == m_windows)
        return;
    m_windows = any;
    updateIdleStop();
    emit windowsChanged();
}

void AndroidApps::updateIdleStop()
{
    const int sec = qEnvironmentVariableIsSet("NAOMI_ANDROID_IDLE_STOP_SEC")
                        ? qEnvironmentVariableIntValue("NAOMI_ANDROID_IDLE_STOP_SEC")
                        : 600;
    if (sec > 0 && m_state == State::On && !m_windows && m_pending.isEmpty())
        m_idleStopTimer.start(sec * 1000);
    else
        m_idleStopTimer.stop();
}

void AndroidApps::sleep(bool on)
{
    if (on == m_asleep || (on && m_state != State::On))
        return;
    m_asleep = on;
    qCInfo(lcAndroid, "Android %s", on ? "засыпает: его окон на столе не видно" : "просыпается");
    emit asleepChanged();
    sendSleep();
}

// Сказать Android (naomid Android.Sleep). Замороженному (стол не смотрят) — когда оттает: команда в его оболочку
// ждала бы ответа и пропала
void AndroidApps::sendSleep()
{
    if (m_state != State::On || m_frozen || m_sleepSent == m_asleep)
        return;
    m_sleepSent = m_asleep;
    QDBusMessage m = QDBusMessage::createMethodCall(kPhoneService, kAndroidPath, kAndroidIface, QStringLiteral("Sleep"));
    m << m_asleep;
    QDBusConnection::systemBus().asyncCall(m);
}

void AndroidApps::stop()
{
    if (m_session) {
        m_session->terminate();   // SIGTERM: сессия гасит контейнер сама
        return;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(kSessionName, QStringLiteral("/SessionManager"),
                                                    QStringLiteral("id.waydro.SessionManager"), QStringLiteral("Stop"));
    QDBusConnection::sessionBus().asyncCall(m, 20000);
}

void AndroidApps::restartStuck(const QString &pkg)
{
    if (m_state != State::On)
        return;
    // открыть заново: окно, что встало, и приложения, которые просили за последнюю минуту (их окна за ним не пришли)
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList apps;
    if (!pkg.isEmpty())
        apps << pkg;
    for (auto it = m_launchedAt.cbegin(); it != m_launchedAt.cend(); ++it)
        if (now - it.value() < 60000 && !apps.contains(it.key()))
            apps << it.key();
    restart(qPrintable(QStringLiteral("окно %1 без картинки 12 с").arg(pkg)), apps);
}

// Перезапуск, когда Android встал: сессию — прочь, приложения apps — в ожидание: sessionGone включит Android снова и,
// когда он готов, откроет их. Дважды за 5 минут не перезапускаем — встало снова, значит, дело не в случайности
void AndroidApps::restart(const char *why, QStringList apps)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_restartedAt && now - m_restartedAt < 5 * 60 * 1000) {
        qCWarning(lcAndroid, "Android встал (%s), но его уже перезапускали %lld с назад — оставляю как есть", why,
                  (long long)((now - m_restartedAt) / 1000));
        return;
    }
    m_restartedAt = now;
    qCWarning(lcAndroid, "Android встал (%s) — перезапускаю, открою снова: %s", why, qPrintable(apps.join(", ")));
    for (const QString &pkg : std::as_const(apps)) {
        if (!m_pending.contains(pkg))
            m_pending << pkg;
        emit launching(pkg);   // плашка «ОТКРЫВАЕТСЯ» — заново на 90 с
    }
    setState(State::Stopping);
    stop();
}

void AndroidApps::shutdown()
{
    if (!m_session)
        return;
    m_session->disconnect(this);
    m_session->terminate();
}

// Файл приложения — ключ-значение в группе [Desktop Entry]; нужны Name и Icon (пакет — из имени файла)
void AndroidApps::readApps()
{
    QVariantList apps;
    const QStringList files = QDir(m_appsDir).entryList({QStringLiteral("waydroid.*.desktop")}, QDir::Files);
    for (const QString &file : files) {
        const QString pkg = file.mid(9, file.size() - 9 - 8);
        if (kHidden.contains(pkg))
            continue;
        QFile f(m_appsDir + "/" + file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QString name, icon;
        bool entry = false;
        for (const QByteArray &raw : f.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.startsWith('[')) {
                entry = line == QLatin1String("[Desktop Entry]");
                continue;
            }
            if (!entry)
                continue;
            if (line.startsWith(QLatin1String("Name=")))
                name = line.mid(5);
            else if (line.startsWith(QLatin1String("Icon=")))
                icon = line.mid(5);
        }
        if (name.isEmpty())
            continue;
        apps.append(QVariantMap{{QStringLiteral("pkg"), pkg}, {QStringLiteral("name"), name},
                                {QStringLiteral("icon"), QFile::exists(icon) ? QUrl::fromLocalFile(icon).toString()
                                                                              : QString()}});
    }
    QCollator collator(QLocale(QStringLiteral("ru_RU")));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(apps.begin(), apps.end(), [&collator](const QVariant &a, const QVariant &b) {
        return collator.compare(a.toMap().value(QStringLiteral("name")).toString(),
                                b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    if (apps == m_apps)
        return;
    m_apps = apps;
    qCInfo(lcAndroid, "приложений Android в списке: %lld", qint64(m_apps.size()));
    emit appsChanged();
}
