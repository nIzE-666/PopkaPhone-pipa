#include "phone_client.h"

#include "dbus_plain.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QLoggingCategory>
#include <QQmlEngine>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPhone, "naomi.phone")

namespace {

const char SERVICE[] = "org.naomi.Phone";
const char PROPS[] = "org.freedesktop.DBus.Properties";

QDBusConnection bus() { return QDBusConnection::systemBus(); }

QString lowerFirst(QString s)
{
    if (!s.isEmpty())
        s[0] = s[0].toLower();
    return s;
}

// массив строк из JS приходит как QVariantList (на шине — av); службе нужен as
QVariantList busArgs(const QVariantList &args)
{
    QVariantList out;
    for (const QVariant &a : args) {
        if (a.metaType() == QMetaType::fromType<QVariantList>()) {
            const QVariantList list = a.toList();
            if (std::all_of(list.cbegin(), list.cend(),
                            [](const QVariant &x) { return x.metaType() == QMetaType::fromType<QString>(); })) {
                out << QVariant(a.toStringList());
                continue;
            }
        }
        // число из JS — целое или дробное смотря как получено (литерал 50 — int, v * 100 — double): на шину — всегда
        // дробным (d), методы службы с числами из QML принимают double (SetVolume, SetRoute)
        if (a.metaType() == QMetaType::fromType<int>() || a.metaType() == QMetaType::fromType<qlonglong>()
            || a.metaType() == QMetaType::fromType<uint>()) {
            out << QVariant(a.toDouble());
            continue;
        }
        out << a;
    }
    return out;
}

} // namespace

BusMirror::BusMirror(const QString &name, const QVariantMap &defaults, const QStringList &signalNames,
                     QObject *parent)
    : QQmlPropertyMap(this, parent)
    , m_path(QStringLiteral("/org/naomi/Phone/") + name)
    , m_iface(QStringLiteral("org.naomi.Phone.") + name)
    , m_signals(signalNames)
{
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it)
        insert(it.key(), it.value());
}

void BusMirror::call(const QString &method, const QVariantList &args, const QJSValue &done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(SERVICE, m_path, m_iface, method);
    msg.setArguments(busArgs(args));
    auto *watcher = new QDBusPendingCallWatcher(bus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, method](QDBusPendingCallWatcher *) {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qCWarning(lcPhone, "%s.%s: %s", qPrintable(m_iface), qPrintable(method), qPrintable(reply.errorMessage()));
            return;
        }
        if (done.isCallable() && m_engine) {
            const QVariant result = reply.arguments().isEmpty() ? QVariant() : plain(reply.arguments().first());
            done.call({m_engine->toScriptValue(result)});
        }
    });
}

void BusMirror::setListening(bool on)
{
    if (on == m_listening)
        return;
    m_listening = on;
    if (on) {
        bus().connect(SERVICE, m_path, PROPS, "PropertiesChanged", this,
                      SLOT(onProps(QString,QVariantMap,QStringList)));
        for (const QString &s : m_signals)
            bus().connect(SERVICE, m_path, m_iface, s, this, SLOT(onSignal(QDBusMessage)));
    } else {
        bus().disconnect(SERVICE, m_path, PROPS, "PropertiesChanged", this,
                         SLOT(onProps(QString,QVariantMap,QStringList)));
        for (const QString &s : m_signals)
            bus().disconnect(SERVICE, m_path, m_iface, s, this, SLOT(onSignal(QDBusMessage)));
    }
}

void BusMirror::refresh()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(SERVICE, m_path, PROPS, "GetAll");
    msg << m_iface;
    auto *watcher = new QDBusPendingCallWatcher(bus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher](QDBusPendingCallWatcher *) {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ReplyMessage)
            apply(plainMap(reply.arguments().value(0)));
    });
}

void BusMirror::onProps(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (iface == m_iface)
        apply(plainMap(QVariant::fromValue(changed)));
}

void BusMirror::onSignal(const QDBusMessage &msg)
{
    QVariantList args;
    for (const QVariant &a : msg.arguments())
        args << plain(a);
    emit busSignal(msg.member(), args);
}

void BusMirror::apply(const QVariantMap &values)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const QString key = lowerFirst(it.key());
        if (value(key) != it.value())
            insert(key, it.value());
    }
}

PhoneClient::PhoneClient()
    : m_battery(new BusMirror("Battery", {{"percent", 0}, {"state", "unknown"}, {"charging", false}}, {}, this))
    , m_charge(new BusMirror("Charge", {{"mode", "daily"}, {"upper", 70}, {"lower", 60}, {"heat", true},
                                        {"defender", true}, {"calibrate", true}, {"lowOff", true}, {"fullOnce", false},
                                        {"state", "battery"}, {"warm", false}, {"stop", 100}, {"start", 0},
                                        {"lastFull", 0LL}, {"nextCalibration", 0LL}}, {}, this))
    , m_wifi(new BusMirror("Wifi", {{"enabled", false}, {"state", "off"}, {"ssid", QString()}, {"strength", 0},
                                    {"networks", QVariantList()}}, {"Failed"}, this))
    , m_cable(new BusMirror("Cable", {{"dock", false}, {"enabled", true}, {"active", false}}, {}, this))
    , m_cell(new BusMirror("Cell", {{"state", "off"}, {"strength", 0}, {"operator", QString()}}, {}, this))
    , m_audio(new BusMirror("Audio", {{"volume", 0}, {"muted", false}, {"output", "other"}, {"outputName", QString()},
                                      {"headphones", QString()}, {"deskVolume", -1}, {"softVolume", false}},
                            {}, this))
    , m_bluetooth(new BusMirror("Bluetooth", {{"powered", false}, {"discovering", false}, {"devices", QVariantList()}},
                                {"Failed", "Pairing"}, this))
    , m_sensors(new BusMirror("Sensors", {{"orientation", "unknown"}, {"light", -1}, {"motion", 0u}}, {}, this))
    , m_perf(new BusMirror("Perf", {{"desk", false}, {"profile", "balanced"}, {"stayAwake", false},
                                    {"deepSuspend", false}, {"thermalLevel", 0}, {"skinTempC", 0.0}}, {}, this))
    , m_system(new BusMirror("System", {}, {}, this))
{
    auto *watcher = new QDBusServiceWatcher(SERVICE, bus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &owner) {
                m_available = !owner.isEmpty();
                emit availableChanged();
                if (m_available)
                    for (BusMirror *m : mirrors())
                        if (m->listening())
                            m->refresh();
            });
    m_available = bus().interface()->isServiceRegistered(SERVICE);
    for (BusMirror *m : mirrors()) {
        m->setListening(true);
        if (m_available)
            m->refresh();
    }
}

void PhoneClient::setEngine(QQmlEngine *engine)
{
    for (BusMirror *m : mirrors())
        m->setEngine(engine);
}

void PhoneClient::setActive(bool on, bool dozing)
{
    for (BusMirror *m : mirrors()) {
        const bool listen = on || (dozing && (m == m_battery || m == m_sensors));
        const bool fresh = listen && !m->listening();
        m->setListening(listen);
        if (fresh && m_available)
            m->refresh();
    }
}
