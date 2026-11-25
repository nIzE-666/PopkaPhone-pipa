#include "app_launcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QProcess>
#include <QTimer>
#include <QWaylandCompositor>
#include <QWaylandClient>
#include <QWaylandSeat>
#include <QWaylandSurface>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcApps, "naomi.apps")

AppLauncher::AppLauncher(const QString &waylandSocket, double density)
    : m_appsDir(QCoreApplication::applicationDirPath() + "/../apps")
    , m_env(QProcessEnvironment::systemEnvironment())
{
    // Окружение программ: Wayland оболочки, GPU через libhybris; ввод текста — модуль «naomi» внутри самой
    // программы (shell/input): клавиатура рисуется в её окне, одним целым с ней (docs/KEYBOARD.md).
    m_env.insert("WAYLAND_DISPLAY", waylandSocket);
    m_env.insert("QT_QPA_PLATFORM", "wayland");
    m_env.insert("EGL_PLATFORM", "wayland");
    m_env.insert("HYBRIS_EGLPLATFORM", "wayland");
    m_env.insert("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    // Дробная плотность 2,75: программа рисует в пикселях панели (буфер 1:1 с экраном), а оболочка
    // показывает окно в своих логических точках — без размытия (docs/SHELL.md, «Окна программ»).
    m_env.insert("QT_SCALE_FACTOR", QString::number(density));
    m_env.insert("QT_SCALE_FACTOR_ROUNDING_POLICY", "PassThrough");
    for (const char *name : {"QT_QPA_EGLFS_INTEGRATION", "QT_PLUGIN_PATH", "NAOMI_TRACE",
                             "NAOMI_FRAMESTATS", "NAOMI_QML"})
        m_env.remove(name);

    // Подгруппа программ стола — внутри своей группы (cgroup v2: строка «0::/путь»); писать в неё можно, только если
    // служба отдала группу нам (Delegate=yes, device/install-shell.sh). Осталась с прошлого запуска замороженной —
    // оттаять: процессов в ней уже нет, но новые попали бы в мороз
    QFile own(QStringLiteral("/proc/self/cgroup"));
    QByteArray group;
    if (own.open(QIODevice::ReadOnly))
        for (const QByteArray &line : own.readAll().split('\n'))
            if (line.startsWith("0::/"))
                group = "/sys/fs/cgroup" + line.mid(3).trimmed() + "/desk";
    if (!group.isEmpty() && (mkdir(group.constData(), 0755) == 0 || errno == EEXIST)
        && writeFile(group + "/cgroup.freeze", "0")) {
        m_deskGroup = group;
        qCInfo(lcApps, "программы стола — в %s (заморозка ядром)", group.constData());
        // программы стола, которых не морозят (Claude), — рядом, в apps; и там и там у каждой — своя подгруппа
        const QByteArray apps = group.left(group.size() - 5) + "/apps";
        if (mkdir(apps.constData(), 0755) == 0 || errno == EEXIST)
            m_appsGroup = apps;
        // подгруппы программ прошлого запуска: systemd, останавливая службу, убил всех — они пусты, убрать
        for (const QByteArray &parent : {m_deskGroup, m_appsGroup}) {
            if (parent.isEmpty())
                continue;
            QDir dir(QString::fromUtf8(parent));
            for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                rmdir(QFile::encodeName(dir.filePath(name)).constData());
        }
    } else {
        qCInfo(lcApps, "своей подгруппы процессов нет (%s) — программы стола ядром не замораживаются",
               group.isEmpty() ? "не cgroup v2" : strerror(errno));
    }

    // Большие ядра для программ стола (docs/WEB.md, «Большие ядра для стола»): группа schedtune desk — её подключает
    // shell-prepare (от root) и отдаёт нам её ручки; подъём — setDeskBoost
    const QByteArray boost = "/dev/stune/desk";
    if (access((boost + "/cgroup.procs").constData(), W_OK) == 0
        && access((boost + "/schedtune.boost").constData(), W_OK) == 0) {
        m_boostGroup = boost;
        writeFile(m_boostGroup + "/schedtune.boost", "0");
        qCInfo(lcApps, "программы стола — в %s (подъём на большие ядра, пока стол смотрят)", boost.constData());
    } else {
        qCInfo(lcApps, "группы schedtune для стола нет (%s) — программы стола без подъёма", boost.constData());
    }
}

void AppLauncher::setDeskBoost(bool on)
{
    if (m_boostGroup.isEmpty() || on == m_deskBoost)
        return;
    if (!writeFile(m_boostGroup + "/schedtune.boost", on ? kDeskBoost : "0")) {
        qCWarning(lcApps, "подъём программ стола: не вышло (%s)", strerror(errno));
        return;
    }
    m_deskBoost = on;
    qCInfo(lcApps, "программы стола — %s", on ? "подъём на большие ядра (стол смотрят)" : "без подъёма");
}

bool AppLauncher::writeFile(const QByteArray &path, const char *value)
{
    const int fd = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::write(fd, value, strlen(value)) == ssize_t(strlen(value));
    ::close(fd);
    return ok;
}

void AppLauncher::freezeDesk(bool on)
{
    if (m_deskGroup.isEmpty() || on == m_deskFrozen)
        return;
    if (!writeFile(m_deskGroup + "/cgroup.freeze", on ? "1" : "0")) {
        qCWarning(lcApps, "программы стола: %s не вышло (%s)", on ? "заморозить" : "оттаять", strerror(errno));
        return;
    }
    m_deskFrozen = on;
    qCInfo(lcApps, "программы стола %s ядром (процессов: %lld)", on ? "заморожены" : "оттаяли",
           qlonglong(groupPids(m_deskGroup).size()));
}

QList<qint64> AppLauncher::groupPids(const QByteArray &dir)
{
    QList<qint64> out;
    if (dir.isEmpty())
        return out;
    QFile procs(QString::fromUtf8(dir + "/cgroup.procs"));
    if (procs.open(QIODevice::ReadOnly))
        for (const QByteArray &line : procs.readAll().split('\n'))
            if (const qint64 pid = line.trimmed().toLongLong(); pid > 0)
                out << pid;
    const QDir sub(QString::fromUtf8(dir));
    for (const QString &name : sub.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        out += groupPids(QFile::encodeName(sub.filePath(name)));
    return out;
}

QByteArray AppLauncher::cgroupOf(qint64 pid)
{
    QFile file(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : file.readAll().split('\n'))
        if (line.startsWith("0::/"))
            return "/sys/fs/cgroup" + line.mid(3).trimmed();
    return {};
}

QByteArray AppLauncher::groupOf(qint64 pid, qint64 *owner) const
{
    if (owner)
        *owner = -1;
    QByteArray group = m_groups.value(pid);
    qint64 who = group.isEmpty() ? -1 : pid;
    if (group.isEmpty()) {
        const QByteArray at = cgroupOf(pid);
        for (auto it = m_groups.constBegin(); it != m_groups.constEnd() && group.isEmpty(); ++it)
            if (at == it.value() || at.startsWith(it.value() + '/')) {
                group = it.value();
                who = it.key();
            }
    }
    if (owner)
        *owner = who;
    return group;
}

void AppLauncher::releaseGroup(qint64 pid)
{
    const QByteArray group = m_groups.value(pid);
    if (group.isEmpty())
        return;
    // процессы в ней ещё есть (ушли от родителя и живут) — подгруппу оставить: по ней их найдёт «закрыть
    // принудительно»; уберёт следующий запуск оболочки
    if (::rmdir(group.constData()) == 0 || errno == ENOENT)
        m_groups.remove(pid);
    else
        qCInfo(lcApps, "pid %lld завершилась, в её подгруппе ещё процессов: %lld", pid, qlonglong(groupPids(group).size()));
}

void AppLauncher::closeAll()
{
    if (m_closing != Closing::No)
        return;
    m_closing = Closing::Waiting;
    freezeDesk(false);   // замороженная программа SIGTERM не прочтёт — и не сохранится
    if (m_running.isEmpty()) {
        finishClosing();
        return;
    }
    for (QProcess *proc : std::as_const(m_running))
        proc->terminate();
    QTimer::singleShot(2000, this, [this] {
        for (QProcess *proc : std::as_const(m_running)) {
            qCWarning(lcApps, "pid %lld не закрылась за 2 с — SIGKILL", proc->processId());
            proc->kill();
            proc->waitForFinished(500);
        }
        finishClosing();
    });
}

void AppLauncher::finishClosing()
{
    if (m_closing == Closing::Done)
        return;
    m_closing = Closing::Done;
    emit allClosed();
}

void AppLauncher::stop(qint64 pid)
{
    if (QProcess *proc = m_running.value(pid))
        proc->terminate();
}

QString AppLauncher::deskApp(qint64 pid) const
{
    const auto known = m_desk.constFind(pid);
    if (known != m_desk.constEnd())
        return *known;
    QFile env(QStringLiteral("/proc/%1/environ").arg(pid));
    if (!env.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &entry : env.readAll().split('\0'))
        if (entry.startsWith("NAOMI_DESK_APP="))
            return QString::fromUtf8(entry.mid(15));
    return {};
}

void AppLauncher::pause(qint64 pid, bool on)
{
    if (m_running.contains(pid))
        ::kill(pid_t(pid), on ? SIGUSR1 : SIGUSR2);
}

qint64 AppLauncher::launch(const QString &app, double scale, const QStringList &args)
{
    return run(app, m_appsDir + "/" + app + "/naomi-" + app, scale, args);
}

qint64 AppLauncher::run(const QString &app, const QString &program, double scale, const QStringList &args,
                         bool freeze)
{
    auto *proc = new QProcess(this);
    QProcessEnvironment env = m_env;
    if (program.startsWith(m_appsDir + u'/')) {
        // Встроенные приложения Naomi линкуют статический плагин ввода naomi-input
        env.insert("QT_IM_MODULE", "naomi");
    } else {
        // Внешние Linux-приложения используют стандартный Wayland text-input-v3
        env.insert("QT_IM_MODULE", "wayland");
        env.insert("QT_IM_MODULES", "wayland");
        env.insert("QT_WAYLAND_TEXT_INPUT_PROTOCOL", "zwp_text_input_v3");
    }
    if (scale > 0) {
        env.insert("QT_SCALE_FACTOR", QString::number(scale));
        env.insert("NAOMI_DESK_APP", app);   // метка программы стола — и её потомкам (deskApp)
    }
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::ForwardedChannels);   // вывод — в журнал оболочки
    const bool desk = scale > 0 && (!m_deskGroup.isEmpty() || !m_boostGroup.isEmpty());
    if (desk)
        freezeDesk(false);   // запускают со стола, который смотрят, — мороза быть не должно
    // своя подгруппа программы — в desk (морозится со столом) или в apps; не вышло — как раньше, в desk или в своей
    QByteArray group;
    if (scale > 0 && !(freeze ? m_deskGroup : m_appsGroup).isEmpty()) {
        group = (freeze ? m_deskGroup : m_appsGroup) + '/' + app.toUtf8() + '-' + QByteArray::number(++m_groupSeq);
        if (mkdir(group.constData(), 0755) != 0 && errno != EEXIST) {
            qCWarning(lcApps, "%s: своей подгруппы нет (%s)", qPrintable(app), strerror(errno));
            group = freeze ? m_deskGroup : QByteArray();
        }
    }
    // В ребёнке после fork (только setpriority/open/write/close):
    // — приоритет — обычный. Поток интерфейса оболочки в режиме стола идёт с nice −5 (naomid, Perf), чтобы такт не
    //   ждал за программами, а ребёнок приоритет наследует: браузер, открытый при смотрящей приставке, получал −5 на
    //   все процессы Chromium и вставал с оболочкой вровень (рентген 19 ноября 2026, docs/XRAY.md);
    // — программа стола — в свою подгруппу (и в группу подъёма schedtune) ещё до exec, со всеми будущими процессами
    //   Chromium. «0» — сам процесс, который пишет
    proc->setChildProcessModifier([procs = group.isEmpty() ? QByteArray() : group + "/cgroup.procs",
                                   boost = desk && !m_boostGroup.isEmpty() ? m_boostGroup + "/cgroup.procs" : QByteArray()] {
        ::setpriority(PRIO_PROCESS, 0, 0);
        for (const QByteArray &path : {procs, boost}) {
            if (path.isEmpty())
                continue;
            const int fd = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                (void)!::write(fd, "0", 1);
                ::close(fd);
            }
        }
    });
    // exec не ждём (waitForStarted): pid есть сразу после fork, а ожидание exec — 20 мс в потоке интерфейса стола
    // (прогон стола 19 ноября 2026, выборки стека: QProcess::waitForStarted → ppoll). Не запустилась — errorOccurred
    proc->start(program, args);
    const qint64 pid = proc->processId();
    if (pid <= 0) {
        qCWarning(lcApps, "%s: не запустилась (%s)", qPrintable(app), qPrintable(proc->errorString()));
        proc->deleteLater();
        if (!group.isEmpty() && group != m_deskGroup)
            ::rmdir(group.constData());
        return -1;
    }
    m_running.insert(pid, proc);
    if (scale > 0)
        m_desk.insert(pid, app);
    if (!group.isEmpty() && group != m_deskGroup)
        m_groups.insert(pid, group);
    auto gone = [this, proc, pid, app](int code) {
        if (!m_running.contains(pid))
            return;
        m_running.remove(pid);
        m_desk.remove(pid);
        releaseGroup(pid);
        proc->deleteLater();
        emit exited(pid, app, code);
        if (m_closing == Closing::Waiting && m_running.isEmpty())
            finishClosing();
    };
    connect(proc, &QProcess::finished, this, [pid, app, gone](int code) {
        qCInfo(lcApps, "%s (pid %lld) завершилась, код %d", qPrintable(app), pid, code);
        gone(code);
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, pid, app, gone](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        qCWarning(lcApps, "%s (pid %lld): не запустилась (%s)", qPrintable(app), pid, qPrintable(proc->errorString()));
        gone(-1);
    });
    qCInfo(lcApps, "%s запущена: pid %lld", qPrintable(app), pid);
    return pid;
}

// Фокус клавиатуры у окна стола (браузер) не трогаем: экран телефона гаснет и уходит домой сам по себе, а на
// столе в это время печатают.
void AppLauncher::releaseFocus()
{
    QWaylandSeat *seat = m_compositor ? m_compositor->defaultSeat() : nullptr;
    if (!seat)
        return;
    const QWaylandSurface *focus = seat->keyboardFocus();
    if (focus && focus->client() && onDesk(focus->client()->processId()))
        return;
    seat->setKeyboardFocus(nullptr);
}
