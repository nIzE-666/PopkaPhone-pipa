// naomid — Wi-Fi через NetworkManager: состояние, сеть, сила сигнала, сети вокруг, управление.
// Всё по сигналам NetworkManager: состояние устройства, активная точка доступа, конец скана, сохранённые
// подключения. Список сетей — дорогой (одним вызовом GetManagedObjects, точек вокруг под сотню, и
// NetworkManager сканирует в фоне сам), поэтому он живой, только пока его смотрят: Scan() — и минуту
// после него список пересобирается после каждого скана. В остальное время служба на сканы не тратится.
// Радио без сна (HoldAwake): энергосбережение Wi-Fi выключено, пока хоть один попросивший жив на шине —
// трансляция на Mac (docs/STREAM.md): со сбережением всё, что идёт к телефону, ждёт его пробуждения.
#pragma once

#include "bus_object.h"

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QTimer>

class Wifi : public BusObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.Wifi")
    Q_PROPERTY(bool Enabled READ enabled)
    Q_PROPERTY(QString State READ state)        // off, disconnected, connecting, connected
    Q_PROPERTY(QString Ssid READ ssid)          // сеть, к которой подключён или подключается
    Q_PROPERTY(int Strength READ strength)      // 0…100
    // сети вокруг: ssid, strength, secure, known (сохранена), active; подключённая — первой, затем
    // сохранённые, затем по силе; одна запись на имя сети, скрытые не показываются. Свежий — минуту после Scan()
    Q_PROPERTY(QVariantList Networks READ networks)
    Q_PROPERTY(bool Awake READ awake)           // энергосбережение радио выключено: его держат (HoldAwake)

public:
    explicit Wifi(QObject *parent = nullptr);

    bool enabled() const { return prop("Enabled").toBool(); }
    QString state() const { return prop("State").toString(); }
    QString ssid() const { return prop("Ssid").toString(); }
    int strength() const { return prop("Strength").toInt(); }
    QVariantList networks() const { return prop("Networks").toList(); }
    bool awake() const { return prop("Awake").toBool(); }

public slots:
    Q_SCRIPTABLE void SetEnabled(bool on);
    Q_SCRIPTABLE void Scan();           // искать сети и держать список живым минуту (звать, пока он на экране)
    // к сохранённой сети — по сохранённому паролю (password пустой) или с новым; к новой — с паролем,
    // если она закрыта. Итог — State, при неудаче — сигнал Failed
    Q_SCRIPTABLE void Connect(const QString &ssid, const QString &password);
    Q_SCRIPTABLE void Disconnect();
    Q_SCRIPTABLE void Forget(const QString &ssid);
    // держать радио без энергосбережения (true), пока зовущий на шине, или отпустить (false)
    Q_SCRIPTABLE void HoldAwake(bool on);

signals:
    // reason: password (не подошёл или нужен), notfound, unsupported (WEP, 802.1X, OWE), timeout, error
    Q_SCRIPTABLE void Failed(const QString &ssid, const QString &reason);

private slots:
    void onManagerProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onDeviceProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onDeviceState(uint newState, uint oldState, uint reason);
    void onApProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onSettingsChanged();
    void onDevicesChanged();            // интерфейс может появиться позже службы

private:
    struct Ap {
        int strength = 0;
        bool secure = false, unsupported = false, sae = false;   // unsupported: WEP, 802.1X, OWE
    };

    void start();                       // NetworkManager появился
    void findDevice();
    void watchDevice(const QString &dev, const QString &iface);
    void loadDevice();
    void loadKnown();
    void loadNetworks();
    void publishNetworks();
    void watchAp(const QString &path);
    void updateState();
    void fail(const QString ssid, const QString &reason);
    bool listWanted() const { return m_listWanted.isValid() && m_listWanted.elapsed() < 60000; }
    void applyPowerSave();              // iw: сбережение выключено, пока есть кто держит; иначе — как обычно (вкл)

    QString m_dev;                      // путь устройства в NetworkManager
    QString m_iface;                    // имя устройства для iw
    quint64 m_discovery = 0;
    QString m_ap;                       // активная точка доступа
    uint m_devState = 0;
    bool m_radio = false;               // WirelessEnabled
    QHash<QString, QStringList> m_known;   // ssid → сохранённые подключения
    QHash<QString, Ap> m_aps;           // ssid → самая сильная точка с этим именем
    QString m_pendingSsid;              // ждём итога Connect
    QString m_pendingNew;               // подключение, созданное этим Connect: при неверном пароле — удалить
    QTimer m_scanDone;                  // пересобрать список не чаще раза в 0,5 с
    QElapsedTimer m_listWanted;         // с последнего Scan()
    QSet<QString> m_holders;            // кто держит радио без сна (имена на шине)
    QDBusServiceWatcher m_holdWatch;    // ушёл с шины (и упал) — отпустить за него
};
