#include "cell.h"
#include "device_profile.h"

#include <QDBusVariant>
#include <QDir>
#include <QLoggingCategory>
#include <QSettings>

Q_LOGGING_CATEGORY(lcCell, "naomid.cell")

namespace {

const char OFONO[] = "org.ofono";
const char MODEM[] = "/ril_0";
const char STORE[] = "/userdata/naomi/state/cell.conf";

} // namespace

Cell::Cell(QObject *parent)
    : BusObject("/org/naomi/Phone/Cell", "org.naomi.Phone.Cell", parent)
{
    set("State", "off");
    set("Strength", 0);
    set("Operator", QString());
    set("Enabled", QSettings(QLatin1String(STORE), QSettings::IniFormat).value("enabled", true).toBool());
    if (!DeviceProfile::current().hasCellular)
        return;

    m_refresh.setSingleShot(true);
    m_refresh.setInterval(100);
    connect(&m_refresh, &QTimer::timeout, this, &Cell::refresh);
    for (const char *iface : {"org.ofono.Modem", "org.ofono.SimManager", "org.ofono.NetworkRegistration"})
        bus().connect(OFONO, MODEM, iface, "PropertyChanged", this, SLOT(onChanged(QString,QDBusVariant)));
    follow(OFONO, [this] { refresh(); });
}

void Cell::onChanged(const QString &name, const QDBusVariant &value)
{
    if (name == "Strength")   // меняется часто — сразу, без перечитывания
        setLevel("Strength", plain(value.variant()).toInt());
    else
        m_refresh.start();
}

// модем → SIM → регистрация: каждый следующий шаг, только если предыдущий в порядке
void Cell::refresh()
{
    callAsync(OFONO, MODEM, "org.ofono.Modem", "GetProperties", {}, [this](const QDBusMessage &r) {
        const QVariantMap modem = r.type() == QDBusMessage::ReplyMessage ? plainMap(r.arguments().value(0))
                                                                         : QVariantMap();
        // модем не в том состоянии, что выбрано (перезагрузка: он поднимается в сети), — поставить; ответ придёт
        // сигналом PropertyChanged — перечитаем
        if (modem.contains("Online") && modem.value("Powered").toBool()
            && modem.value("Online").toBool() != enabled())
            apply();
        if (!modem.value("Online").toBool()) {
            set("State", "off");
            set("Strength", 0);
            set("Operator", QString());
            return;
        }
        callAsync(OFONO, MODEM, "org.ofono.SimManager", "GetProperties", {}, [this](const QDBusMessage &r) {
            const QVariantMap sim = r.type() == QDBusMessage::ReplyMessage ? plainMap(r.arguments().value(0))
                                                                           : QVariantMap();
            if (!sim.value("Present").toBool()) {
                set("State", "nosim");
                set("Strength", 0);
                set("Operator", QString());
                return;
            }
            callAsync(OFONO, MODEM, "org.ofono.NetworkRegistration", "GetProperties", {},
                      [this](const QDBusMessage &r) {
                          const QVariantMap reg = r.type() == QDBusMessage::ReplyMessage
                                                      ? plainMap(r.arguments().value(0))
                                                      : QVariantMap();
                          set("State", reg.value("Status", "searching").toString());
                          setLevel("Strength", reg.value("Strength").toInt());
                          set("Operator", reg.value("Name").toString());
                      });
        });
    });
}

void Cell::SetEnabled(bool on)
{
    if (on != enabled()) {
        QDir().mkpath(QStringLiteral("/userdata/naomi/state"));
        QSettings(QLatin1String(STORE), QSettings::IniFormat).setValue("enabled", on);
        set("Enabled", on);
        qCInfo(lcCell, "модем: %s", on ? "включён" : "выключен (не ищет сеть)");
    }
    apply();
}

void Cell::apply()
{
    const bool on = enabled();
    callAsync(OFONO, MODEM, "org.ofono.Modem", "SetProperty",
              {QStringLiteral("Online"), QVariant::fromValue(QDBusVariant(on))}, [on](const QDBusMessage &r) {
                  if (r.type() == QDBusMessage::ErrorMessage)
                      qCWarning(lcCell, "модем: Online = %d не принят: %s", on, qPrintable(r.errorMessage()));
              });
}
