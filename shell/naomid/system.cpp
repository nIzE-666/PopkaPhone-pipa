#include "system.h"

#include "device_profile.h"

#include <QDBusError>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSystem, "naomid.system")

System::System(QObject *parent)
    : BusObject("/org/naomi/Phone/System", "org.naomi.Phone.System", parent)
{
    m_oneUi.setStandardInputFile(QProcess::nullDevice());
    m_oneUi.setStandardOutputFile(QProcess::nullDevice());
    m_oneUiTimeout.setSingleShot(true);
    m_oneUiTimeout.setInterval(20000);
    connect(&m_oneUiTimeout, &QTimer::timeout, this, [this] {
        if (m_oneUi.state() == QProcess::NotRunning)
            return;
        m_oneUi.kill();
        replyToOneUi(QStringLiteral("Помощник One UI не ответил вовремя (таймаут 20 с)."));
    });
    connect(&m_oneUi, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_oneUiTimeout.stop();
            replyToOneUi(QStringLiteral("Не удалось запустить помощник One UI. %1").arg(m_oneUi.errorString()));
        }
    });
    connect(&m_oneUi, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_oneUiTimeout.stop();
        if (m_oneUiRequest.type() == QDBusMessage::InvalidMessage)
            return;
        if (status == QProcess::NormalExit && code == 0) {
            replyToOneUi({});
            return;
        }
        QString error = status == QProcess::CrashExit ? QStringLiteral("Помощник One UI завершился аварийно.")
                                                       : QStringLiteral("Помощник One UI завершился с кодом %1.").arg(code);
        const QString detail = QString::fromUtf8(m_oneUi.readAllStandardError()).simplified().left(2048);
        if (!detail.isEmpty())
            error += u' ' + detail;
        replyToOneUi(error);
    });
}

// systemctl — отдельным процессом: ответ на шину уходит сразу, телефон перезагружается следом
void System::Reboot()
{
    qCInfo(lcSystem, "перезагрузка телефона — просьба с шины");
    QProcess::startDetached("env", {"SYSTEMCTL_SKIP_AUTO_KEXEC=1", "SYSTEMCTL_SKIP_AUTO_SOFT_REBOOT=1",
                                    "systemctl", "reboot"});
}

void System::PowerOff()
{
    qCInfo(lcSystem, "выключение телефона — просьба с шины");
    QProcess::startDetached("env", {"SYSTEMCTL_SKIP_AUTO_KEXEC=1", "SYSTEMCTL_SKIP_AUTO_SOFT_REBOOT=1",
                                    "systemctl", "poweroff"});
}

void System::RebootOneUi(const QDBusMessage &request)
{
    request.setDelayedReply(true);
    if (QLatin1String(DeviceProfile::current().name) != QLatin1String("pipa")) {
        bus().send(request.createErrorReply(QDBusError::NotSupported, QStringLiteral("One UI настроена только на pipa.")));
        return;
    }
    if (m_oneUi.state() != QProcess::NotRunning) {
        bus().send(request.createErrorReply(QDBusError::LimitsExceeded, QStringLiteral("Переключение уже выполняется.")));
        return;
    }
    m_oneUiRequest = request;
    m_oneUiTimeout.start();
    m_oneUi.start(QStringLiteral("/usr/local/lib/naomi/naomi-pipa-slot-switch"), {QStringLiteral("one-ui")});
}

void System::replyToOneUi(const QString &error)
{
    if (error.isEmpty()) {
        qCInfo(lcSystem, "запрошена перезагрузка в One UI, выбран слот _a");
        bus().send(m_oneUiRequest.createReply());
    } else {
        qCWarning(lcSystem, "переход в One UI: %s", qPrintable(error));
        bus().send(m_oneUiRequest.createErrorReply(QDBusError::Failed, error));
    }
    m_oneUiRequest = {};
}
