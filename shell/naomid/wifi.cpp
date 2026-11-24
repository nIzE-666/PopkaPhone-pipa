#include "wifi.h"

#include <QProcess>

#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QLoggingCategory>

#include <algorithm>
#include <memory>

Q_LOGGING_CATEGORY(lcWifi, "naomid.wifi")

using NmSettings = QMap<QString, QVariantMap>;   // a{sa{sv}} — настройки подключения NetworkManager
Q_DECLARE_METATYPE(NmSettings)

namespace {

const char NM[] = "org.freedesktop.NetworkManager";
const char NM_PATH[] = "/org/freedesktop/NetworkManager";
const char NM_SETTINGS[] = "/org/freedesktop/NetworkManager/Settings";
const char SETTINGS_IFACE[] = "org.freedesktop.NetworkManager.Settings";
const char CONN_IFACE[] = "org.freedesktop.NetworkManager.Settings.Connection";
const char DEV_IFACE[] = "org.freedesktop.NetworkManager.Device";
const char WIFI_IFACE[] = "org.freedesktop.NetworkManager.Device.Wireless";
const char AP_IFACE[] = "org.freedesktop.NetworkManager.AccessPoint";
const char PROPS[] = "org.freedesktop.DBus.Properties";
const char OBJECT_MANAGER[] = "org.freedesktop.DBus.ObjectManager";
const char OBJECT_MANAGER_PATH[] = "/org/freedesktop";
constexpr uint kWifiDeviceType = 2;   // NM_DEVICE_TYPE_WIFI

// состояния устройства NetworkManager (NMDeviceState)
enum : uint { DevUnmanaged = 10, DevPrepare = 40, DevActivated = 100, DevFailed = 120 };
// флаги точки доступа (NM80211ApSecurityFlags, NM80211ApFlags)
enum : uint { Privacy = 0x1, KeyPsk = 0x100, Key8021x = 0x200, KeySae = 0x400, KeyOwe = 0x800 };

QVariant path(const QString &p) { return QVariant::fromValue(QDBusObjectPath(p.isEmpty() ? "/" : p)); }

} // namespace

Wifi::Wifi(QObject *parent)
    : BusObject("/org/naomi/Phone/Wifi", "org.naomi.Phone.Wifi", parent)
{
    qDBusRegisterMetaType<NmSettings>();
    set("Enabled", false);
    set("State", "off");
    set("Ssid", QString());
    set("Strength", 0);
    set("Networks", QVariantList());
    set("Awake", false);

    m_holdWatch.setConnection(bus());
    m_holdWatch.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_holdWatch, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &who) {
        m_holdWatch.removeWatchedService(who);
        if (m_holders.remove(who)) {
            qCInfo(lcWifi, "радио без сна: %s ушёл с шины — отпускаю", qPrintable(who));
            applyPowerSave();
        }
    });

    m_scanDone.setSingleShot(true);
    m_scanDone.setInterval(500);
    connect(&m_scanDone, &QTimer::timeout, this, &Wifi::loadNetworks);

    bus().connect(NM, NM_PATH, PROPS, "PropertiesChanged", this,
                  SLOT(onManagerProps(QString,QVariantMap,QStringList)));
    bus().connect(NM, NM_PATH, NM, "DeviceAdded", this, SLOT(onDevicesChanged()));
    bus().connect(NM, NM_PATH, NM, "DeviceRemoved", this, SLOT(onDevicesChanged()));
    bus().connect(NM, NM_SETTINGS, SETTINGS_IFACE, "NewConnection", this, SLOT(onSettingsChanged()));
    bus().connect(NM, NM_SETTINGS, SETTINGS_IFACE, "ConnectionRemoved", this, SLOT(onSettingsChanged()));
    follow(NM, [this] { start(); });
}

void Wifi::start()
{
    watchDevice({}, {});
    callAsync(NM, NM_PATH, PROPS, "Get", {NM, "WirelessEnabled"}, [this](const QDBusMessage &r) {
        if (r.type() != QDBusMessage::ReplyMessage)
            return;
        m_radio = plain(r.arguments().value(0)).toBool();
        updateState();
    });
    findDevice();
    loadKnown();
}

void Wifi::findDevice()
{
    const auto discovery = ++m_discovery;
    callAsync(NM, OBJECT_MANAGER_PATH, OBJECT_MANAGER, "GetManagedObjects", {}, [this, discovery](const QDBusMessage &r) {
        if (discovery != m_discovery)
            return;
        QString dev, iface;
        int priority = 0;
        if (r.type() == QDBusMessage::ReplyMessage) {
            const auto objects = plainMap(r.arguments().value(0));
            for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                const auto props = it.value().toMap().value(DEV_IFACE).toMap();
                const QString name = props.value("Interface").toString();
                if (props.value("DeviceType").toUInt() != kWifiDeviceType || !props.value("Managed").toBool()
                    || name.isEmpty() || name == "wlan1" || name.startsWith("p2p"))
                    continue;
                const bool connected = props.value("State").toUInt() == DevActivated;
                const int rank = 1 + (connected ? 2 : 0) + (name == "wlan0" ? 1 : 0);
                if (rank > priority) {
                    priority = rank;
                    dev = it.key();
                    iface = name;
                }
            }
        }
        watchDevice(dev, iface);
    });
}

void Wifi::watchDevice(const QString &dev, const QString &iface)
{
    if (dev != m_dev || iface != m_iface) {
        if (!m_dev.isEmpty()) {
            bus().disconnect(NM, m_dev, PROPS, "PropertiesChanged", this,
                             SLOT(onDeviceProps(QString,QVariantMap,QStringList)));
            bus().disconnect(NM, m_dev, DEV_IFACE, "StateChanged", this, SLOT(onDeviceState(uint,uint,uint)));
        }
        watchAp({});
        m_aps.clear();
        m_devState = 0;
        if (!m_pendingSsid.isEmpty())
            fail(m_pendingSsid, "error");
        m_dev = dev;
        m_iface = iface;
        set("Awake", false);
        if (!m_dev.isEmpty()) {
            bus().connect(NM, m_dev, PROPS, "PropertiesChanged", this,
                          SLOT(onDeviceProps(QString,QVariantMap,QStringList)));
            bus().connect(NM, m_dev, DEV_IFACE, "StateChanged", this, SLOT(onDeviceState(uint,uint,uint)));
            qCInfo(lcWifi, "устройство Wi-Fi: %s", qPrintable(m_iface));
        }
        updateState();
    }
    if (!m_dev.isEmpty()) {
        loadDevice();
        loadNetworks();
    }
}

void Wifi::loadDevice()
{
    callAsync(NM, m_dev, PROPS, "Get", {DEV_IFACE, "State"}, [this, dev = m_dev](const QDBusMessage &r) {
        if (dev != m_dev || r.type() != QDBusMessage::ReplyMessage)
            return;
        m_devState = plain(r.arguments().value(0)).toUInt();
        updateState();
        if (m_devState == DevActivated && !m_holders.isEmpty() && !awake())
            applyPowerSave();
    });
    callAsync(NM, m_dev, PROPS, "Get", {WIFI_IFACE, "ActiveAccessPoint"}, [this, dev = m_dev](const QDBusMessage &r) {
        if (dev == m_dev && r.type() == QDBusMessage::ReplyMessage)
            watchAp(plain(r.arguments().value(0)).toString());
    });
}

// сохранённые подключения Wi-Fi: имя сети → пути (их немного, читаем каждое)
void Wifi::loadKnown()
{
    callAsync(NM, NM_SETTINGS, SETTINGS_IFACE, "ListConnections", {}, [this](const QDBusMessage &r) {
        if (r.type() != QDBusMessage::ReplyMessage)
            return;
        const QVariantList paths = plain(r.arguments().value(0)).toList();
        auto known = std::make_shared<QHash<QString, QStringList>>();
        auto left = std::make_shared<int>(paths.size());
        if (paths.isEmpty()) {
            m_known.clear();
            publishNetworks();
        }
        for (const QVariant &p : paths) {
            const QString conn = p.toString();
            callAsync(NM, conn, CONN_IFACE, "GetSettings", {}, [this, conn, known, left](const QDBusMessage &r) {
                if (r.type() == QDBusMessage::ReplyMessage) {
                    const QVariantMap s = plainMap(r.arguments().value(0));
                    if (s.value("connection").toMap().value("type") == "802-11-wireless") {
                        const QByteArray ssid = s.value("802-11-wireless").toMap().value("ssid").toByteArray();
                        (*known)[QString::fromUtf8(ssid)] << conn;
                    }
                }
                if (--*left == 0) {
                    m_known = *known;
                    publishNetworks();
                }
            });
        }
    });
}

// Точки доступа выбранного интерфейса — одним вызовом ObjectManager.
void Wifi::loadNetworks()
{
    if (m_dev.isEmpty())
        return;
    callAsync(NM, OBJECT_MANAGER_PATH, OBJECT_MANAGER, "GetManagedObjects", {},
              [this, watched = m_dev](const QDBusMessage &r) {
                  if (watched != m_dev || r.type() != QDBusMessage::ReplyMessage)
                      return;
                  const QVariantMap objects = plain(r.arguments().value(0)).toMap();
                  const QVariantMap dev = objects.value(m_dev).toMap().value(WIFI_IFACE).toMap();
                  QHash<QString, Ap> aps;
                  for (const QVariant &p : dev.value("AccessPoints").toList()) {
                      const QVariantMap ap = objects.value(p.toString()).toMap().value(AP_IFACE).toMap();
                      const QString ssid = QString::fromUtf8(ap.value("Ssid").toByteArray());
                      if (ssid.isEmpty())
                          continue;
                      const uint keys = ap.value("WpaFlags").toUInt() | ap.value("RsnFlags").toUInt();
                      Ap a;
                      a.strength = ap.value("Strength").toInt();
                      a.secure = keys || (ap.value("Flags").toUInt() & Privacy);
                      // поддерживаем открытые, WPA/WPA2-PSK и WPA3-SAE; WEP, 802.1X и OWE — нет
                      a.unsupported = (keys & (Key8021x | KeyOwe)) || (a.secure && !keys);
                      a.sae = (keys & KeySae) && !(keys & KeyPsk);
                      if (!aps.contains(ssid) || aps[ssid].strength < a.strength)
                          aps[ssid] = a;
                  }
                  m_aps = aps;
                  publishNetworks();
              });
}

void Wifi::publishNetworks()
{
    const QString current = state() == "connected" || state() == "connecting" ? ssid() : QString();
    QVariantList list;
    for (auto it = m_aps.cbegin(); it != m_aps.cend(); ++it) {
        // у подключённой — сила её точки (как в строке состояния), у остальных — самой сильной точки с этим именем
        const bool active = it.key() == current;
        list << QVariantMap{{"ssid", it.key()}, {"strength", active ? strength() : it->strength},
                            {"secure", it->secure}, {"known", m_known.contains(it.key())}, {"active", active}};
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        for (const char *key : {"active", "known"})
            if (x.value(key) != y.value(key))
                return x.value(key).toBool();
        if (x.value("strength") != y.value("strength"))
            return x.value("strength").toInt() > y.value("strength").toInt();
        return x.value("ssid").toString() < y.value("ssid").toString();
    });
    set("Networks", list);
}

void Wifi::watchAp(const QString &ap)
{
    const QString next = ap == "/" ? QString() : ap;
    if (next == m_ap)
        return;
    if (!m_ap.isEmpty())
        bus().disconnect(NM, m_ap, PROPS, "PropertiesChanged", this, SLOT(onApProps(QString,QVariantMap,QStringList)));
    m_ap = next;
    if (m_ap.isEmpty())
        return;
    bus().connect(NM, m_ap, PROPS, "PropertiesChanged", this, SLOT(onApProps(QString,QVariantMap,QStringList)));
    callAsync(NM, m_ap, PROPS, "GetAll", {AP_IFACE}, [this, ap = m_ap](const QDBusMessage &r) {
        if (ap != m_ap || r.type() != QDBusMessage::ReplyMessage)
            return;
        const QVariantMap p = plainMap(r.arguments().value(0));
        set("Ssid", QString::fromUtf8(p.value("Ssid").toByteArray()));
        setLevel("Strength", p.value("Strength").toInt());
        publishNetworks();
    });
}

void Wifi::updateState()
{
    QString s = "disconnected";
    if (!m_radio || m_devState <= DevUnmanaged)
        s = "off";
    else if (m_devState == DevActivated)
        s = "connected";
    else if (m_devState >= DevPrepare && m_devState < DevActivated)
        s = "connecting";
    set("Enabled", m_radio);
    set("State", s);
    if (s == "off" || s == "disconnected") {
        set("Ssid", QString());
        set("Strength", 0);
    } else if (ssid().isEmpty() && !m_pendingSsid.isEmpty()) {
        set("Ssid", m_pendingSsid);
    }
    publishNetworks();
}

void Wifi::onManagerProps(const QString &, const QVariantMap &changed, const QStringList &)
{
    if (changed.contains("WirelessEnabled")) {
        m_radio = plain(changed.value("WirelessEnabled")).toBool();
        updateState();
    }
    if (changed.contains("PrimaryConnection") || (changed.contains("WirelessEnabled") && m_radio))
        findDevice();
}

void Wifi::onDeviceProps(const QString &, const QVariantMap &changed, const QStringList &)
{
    if (changed.contains("Managed") || changed.contains("Interface"))
        findDevice();
    if (changed.contains("ActiveAccessPoint"))
        watchAp(plain(changed.value("ActiveAccessPoint")).toString());
    if ((changed.contains("LastScan") || changed.contains("AccessPoints")) && listWanted())
        m_scanDone.start();
}

void Wifi::onDeviceState(uint newState, uint, uint reason)
{
    m_devState = newState;
    updateState();
    if (newState == DevActivated && !m_holders.isEmpty())
        applyPowerSave();   // драйвер при каждом подключении возвращает сбережение — выключить снова
    if (m_pendingSsid.isEmpty())
        return;
    if (newState == DevActivated) {
        qCInfo(lcWifi, "подключено: %s", qPrintable(m_pendingSsid));
        m_pendingSsid.clear();
        m_pendingNew.clear();
    } else if (newState == DevFailed) {
        // NMDeviceStateReason: 7 — нет секретов (пароль не подошёл, спросить некому), 8 — отказ
        // при рукопожатии, 11 — истекло время
        const QString why = reason == 7 || reason == 8 ? "password" : reason == 11 ? "timeout" : "error";
        if (why == "password" && !m_pendingNew.isEmpty())
            callAsync(NM, m_pendingNew, CONN_IFACE, "Delete", {});   // не хранить неверный пароль
        qCInfo(lcWifi, "не подключиться к %s: %s (причина NetworkManager %u)", qPrintable(m_pendingSsid),
               qPrintable(why), reason);
        fail(m_pendingSsid, why);
    }
}

void Wifi::onApProps(const QString &, const QVariantMap &changed, const QStringList &)
{
    if (!changed.contains("Strength"))
        return;
    setLevel("Strength", plain(changed.value("Strength")).toInt());
    publishNetworks();
}

void Wifi::onSettingsChanged()
{
    loadKnown();
}

void Wifi::onDevicesChanged()
{
    findDevice();
}

void Wifi::fail(const QString ssid, const QString &reason)
{
    // ssid — копией: зовут и с самим m_pendingSsid, который тут же стирается (по ссылке сигнал уходил с пустым
    // именем — экран и окно Wi-Fi не знали, к какой сети не подошёл пароль)
    if (ssid == m_pendingSsid) {
        m_pendingSsid.clear();
        m_pendingNew.clear();
    }
    emit Failed(ssid, reason);
}

void Wifi::SetEnabled(bool on)
{
    callAsync(NM, NM_PATH, PROPS, "Set", {NM, "WirelessEnabled", QVariant::fromValue(QDBusVariant(on))});
}

void Wifi::Scan()
{
    if (m_dev.isEmpty())
        return;
    m_listWanted.start();
    loadNetworks();   // то, что NetworkManager уже знает, — сразу, свежий скан догонит
    // «рано, только что сканировали» — не ошибка: список и так свежий
    callAsync(NM, m_dev, WIFI_IFACE, "RequestScan", {QVariant::fromValue(QVariantMap())});
}

void Wifi::Connect(const QString &ssid, const QString &password)
{
    // точку доступа выбирает NetworkManager (самую сильную с этим именем): наш список может быть несвежим
    const QStringList known = m_known.value(ssid);
    const Ap ap = m_aps.value(ssid);
    if (m_dev.isEmpty() || (known.isEmpty() && !m_aps.contains(ssid))) {
        fail(ssid, "notfound");
        return;
    }
    m_pendingSsid = ssid;
    m_pendingNew.clear();
    qCInfo(lcWifi, "подключаюсь к %s (%s)", qPrintable(ssid),
           !known.isEmpty() && password.isEmpty() ? "сохранённая" : "новый пароль");
    if (!known.isEmpty() && password.isEmpty()) {
        callAsync(NM, NM_PATH, NM, "ActivateConnection", {path(known.first()), path(m_dev), path({})},
                  [this, ssid](const QDBusMessage &r) {
                      if (r.type() == QDBusMessage::ErrorMessage)
                          fail(ssid, "error");
                  });
        return;
    }
    if (ap.unsupported) {
        fail(ssid, "unsupported");
        return;
    }
    if (ap.secure && password.isEmpty()) {
        fail(ssid, "password");
        return;
    }
    for (const QString &old : known)   // новый пароль — новое подключение вместо старого
        callAsync(NM, old, CONN_IFACE, "Delete", {});
    NmSettings s;
    s["connection"] = QVariantMap{{"id", ssid}, {"type", "802-11-wireless"}};
    s["802-11-wireless"] = QVariantMap{{"ssid", ssid.toUtf8()}, {"mode", "infrastructure"}};
    if (ap.secure)
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", ap.sae ? "sae" : "wpa-psk"}, {"psk", password}};
    callAsync(NM, NM_PATH, NM, "AddAndActivateConnection", {QVariant::fromValue(s), path(m_dev), path({})},
              [this, ssid](const QDBusMessage &r) {
                  if (r.type() == QDBusMessage::ErrorMessage)
                      fail(ssid, "error");
                  else if (m_pendingSsid == ssid)
                      m_pendingNew = plain(r.arguments().value(0)).toString();
              });
}

void Wifi::Disconnect()
{
    if (!m_dev.isEmpty())
        callAsync(NM, m_dev, DEV_IFACE, "Disconnect", {});
}

void Wifi::Forget(const QString &ssid)
{
    for (const QString &conn : m_known.value(ssid))
        callAsync(NM, conn, CONN_IFACE, "Delete", {});
}

// ---------- радио без сна ----------

void Wifi::HoldAwake(bool on)
{
    const QString who = calledFromDBus() ? message().service() : QStringLiteral("naomid");
    if (on) {
        m_holders.insert(who);
        if (calledFromDBus() && !m_holdWatch.watchedServices().contains(who))
            m_holdWatch.addWatchedService(who);
    } else {
        m_holders.remove(who);
        m_holdWatch.removeWatchedService(who);
    }
    applyPowerSave();
}

// Сбережение — через nl80211 (iw), как его ставит драйвер: настройка подключения в NetworkManager
// (802-11-wireless.powersave) применяется только с переподключением. Проверено 13 ноября 2026: пинг с
// телефона до роутера со сбережением — в среднем 18,5 мс (до 52), без него — 4 мс (до 9).
void Wifi::applyPowerSave()
{
    if (m_iface.isEmpty())
        return;
    const bool awake = !m_holders.isEmpty();
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, awake](int code) {
        if (code != 0)
            qCWarning(lcWifi, "iw power_save %s: %s", awake ? "off" : "on", p->readAllStandardError().trimmed().constData());
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            p->deleteLater();
    });
    p->start(QStringLiteral("/usr/sbin/iw"), {"dev", m_iface, "set", "power_save", awake ? "off" : "on"});
    if (awake != prop("Awake").toBool())
        qCInfo(lcWifi, "радио без сна: %s", awake ? "да" : "нет");
    set("Awake", awake);
}
