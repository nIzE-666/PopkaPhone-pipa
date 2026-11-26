#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class SystemTools : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool vpnAvailable READ vpnAvailable CONSTANT)
    Q_PROPERTY(bool androidAvailable READ androidAvailable CONSTANT)
    Q_PROPERTY(QStringList vpnProfiles READ vpnProfiles NOTIFY changed)
    Q_PROPERTY(QString vpnStatus READ vpnStatus NOTIFY changed)
    Q_PROPERTY(QString vpnError READ vpnError NOTIFY changed)
    Q_PROPERTY(QString androidError READ androidError NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList linuxApps READ linuxApps NOTIFY linuxAppsChanged)
    Q_PROPERTY(QStringList clipboardHistory READ clipboardHistory NOTIFY clipboardChanged)
    Q_PROPERTY(QString pcClipboardStatus READ pcClipboardStatus NOTIFY clipboardChanged)
    Q_PROPERTY(bool pcClipboardBusy READ pcClipboardBusy NOTIFY clipboardChanged)
    Q_PROPERTY(bool hardwareKeyboard READ hardwareKeyboard NOTIFY hardwareChanged)
    Q_PROPERTY(bool externalDisplay READ externalDisplay NOTIFY hardwareChanged)
    Q_PROPERTY(QString powerProfile READ powerProfile NOTIFY systemChanged)
    Q_PROPERTY(QVariantList services READ services NOTIFY systemChanged)
    Q_PROPERTY(QVariantMap sysInfo READ sysInfo NOTIFY systemChanged)
    Q_PROPERTY(QString systemStatus READ systemStatus NOTIFY systemChanged)
    Q_PROPERTY(bool systemBusy READ systemBusy NOTIFY systemChanged)

public:
    explicit SystemTools(bool tablet);

    bool vpnAvailable() const { return m_vpnAvailable; }
    bool androidAvailable() const { return m_androidAvailable; }
    const QStringList &vpnProfiles() const { return m_profiles; }
    const QString &vpnStatus() const { return m_status; }
    const QString &vpnError() const { return m_vpnError; }
    const QString &androidError() const { return m_androidError; }
    bool busy() const { return m_operation != Operation::Idle; }
    const QVariantList &linuxApps() const { return m_linuxApps; }
    const QStringList &clipboardHistory() const { return m_clipboardHistory; }
    const QString &pcClipboardStatus() const { return m_pcClipboardStatus; }
    bool pcClipboardBusy() const { return m_pcClipboardBusy; }
    bool hardwareKeyboard() const { return m_hardwareKeyboard; }
    bool externalDisplay() const { return m_externalDisplay; }
    const QString &powerProfile() const { return m_powerProfile; }
    const QVariantList &services() const { return m_services; }
    const QVariantMap &sysInfo() const { return m_sysInfo; }
    const QString &systemStatus() const { return m_systemStatus; }
    bool systemBusy() const { return m_sysBusy; }

    Q_INVOKABLE void refreshVpn();
    Q_INVOKABLE void setVpn(const QString &profile);
    Q_INVOKABLE void bootAndroid();
    Q_INVOKABLE void refreshLinuxApps();
    Q_INVOKABLE void copyClipboard(const QString &text);
    Q_INVOKABLE void removeClipboardAt(int index);
    Q_INVOKABLE void clearClipboardHistory();
    Q_INVOKABLE void pullPcClipboard();
    Q_INVOKABLE void pushPcClipboard(const QString &text = QString());
    Q_INVOKABLE void refreshHardwareState();
    Q_INVOKABLE void refreshSystemInfo();
    Q_INVOKABLE void setPowerProfile(const QString &profile);
    Q_INVOKABLE void setStayAwake(bool on);
    Q_INVOKABLE void setDeepSuspend(bool on);
    Q_INVOKABLE void toggleService(const QString &id, bool enable);
    Q_INVOKABLE void cleanJournal();
    Q_INVOKABLE void setTimezone(const QString &tz);
    Q_INVOKABLE void syncTime();
    void setActive(bool on);

signals:
    void changed();
    void linuxAppsChanged();
    void clipboardChanged();
    void hardwareChanged();
    void systemChanged();

private:
    enum class Operation { Idle, List, Status, VpnChange, Reboot };
    void run(Operation op, const QString &program, const QStringList &arguments);
    void complete(bool ok);
    void recordClipboard(const QString &text);
    void loadClipboardHistory();
    void saveClipboardHistory() const;
    bool readPcAgentConfig(QString &baseUrl, QString &token) const;
    void runSystemCommand(const QString &program, const QStringList &arguments, const QString &okMsg, const QString &errMsg);

    const bool m_vpnAvailable, m_androidAvailable;
    Operation m_operation = Operation::Idle;
    QProcess m_process;
    QTimer m_timeout;
    QTimer m_hwTimer;
    QNetworkAccessManager m_net;
    QStringList m_profiles;
    QString m_status, m_vpnError, m_androidError;
    QVariantList m_linuxApps;
    QStringList m_clipboardHistory;
    QString m_pcClipboardStatus;
    bool m_pcClipboardBusy = false;
    bool m_ignoreClipboard = false;
    bool m_hardwareKeyboard = false;
    bool m_externalDisplay = false;
    QString m_powerProfile;
    QVariantList m_services;
    QVariantMap m_sysInfo;
    QString m_systemStatus;
    bool m_sysBusy = false;
};
