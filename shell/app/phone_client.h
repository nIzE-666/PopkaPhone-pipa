// НаомиФон — служба железа naomid (docs/NAOMID.md) для QML: naomiPhone.battery.percent,
// naomiPhone.wifi.networks, naomiPhone.wifi.call("Connect", [ssid, пароль]).
// Каждый объект службы зеркалится в QML как есть: свойства (с маленькой буквы) обновляются по сигналу
// PropertiesChanged, методы зовутся через call(), сигналы приходят как busSignal(имя, аргументы).
// Новая часть железа в naomid не требует кода здесь — только строчки в PhoneClient. Массив строк из
// JS уходит на шину как список строк (as): naomiPhone.sensors.call("Watch", [["light"]]).
// Экран погашен (active = false) — зеркала не слушают шину и процесс оболочки не просыпается;
// при включении перечитывают всё. В AOD слушают только батарея (заряд и зарядка — на экране AOD) и датчики:
// со второй ступенью AOD — движение (свет в AOD стоит, так что сигналы — только когда телефон сдвинули).
#pragma once

#include <QDBusMessage>
#include <QJSValue>
#include <QQmlPropertyMap>

class QQmlEngine;

class BusMirror : public QQmlPropertyMap
{
    Q_OBJECT
public:
    BusMirror(const QString &name, const QVariantMap &defaults, const QStringList &signalNames,
              QObject *parent);

    // вызвать метод объекта; done(результат) — когда служба ответит (при ошибке не зовётся)
    Q_INVOKABLE void call(const QString &method, const QVariantList &args = {}, const QJSValue &done = {});

    void setEngine(QQmlEngine *engine) { m_engine = engine; }
    void setListening(bool on);
    bool listening() const { return m_listening; }
    void refresh();

signals:
    void busSignal(const QString &name, const QVariantList &args);

private slots:
    void onProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onSignal(const QDBusMessage &msg);

private:
    void apply(const QVariantMap &values);

    QString m_path, m_iface;
    QStringList m_signals;
    QQmlEngine *m_engine = nullptr;
    bool m_listening = false;
};

class PhoneClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *battery READ battery CONSTANT)
    // заряд в коридоре по режиму (docs/NAOMID.md, «Заряд»): mode, state, stop/start, call("SetMode", ["dock"])
    Q_PROPERTY(QObject *charge READ charge CONSTANT)
    Q_PROPERTY(QObject *wifi READ wifi CONSTANT)
    // интернет по кабелю через приставку (docs/PI.md): dock — приставка на кабеле, enabled — разрешён, active — идёт
    Q_PROPERTY(QObject *cable READ cable CONSTANT)
    Q_PROPERTY(QObject *cell READ cell CONSTANT)
    Q_PROPERTY(QObject *audio READ audio CONSTANT)
    Q_PROPERTY(QObject *bluetooth READ bluetooth CONSTANT)
    Q_PROPERTY(QObject *sensors READ sensors CONSTANT)
    Q_PROPERTY(QObject *perf READ perf CONSTANT)
    Q_PROPERTY(QObject *system READ system CONSTANT)   // телефон целиком: call("Reboot"), call("PowerOff")
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)   // naomid работает

public:
    PhoneClient();

    QObject *battery() const { return m_battery; }
    QObject *charge() const { return m_charge; }
    QObject *wifi() const { return m_wifi; }
    QObject *cable() const { return m_cable; }
    QObject *cell() const { return m_cell; }
    QObject *audio() const { return m_audio; }
    QObject *bluetooth() const { return m_bluetooth; }
    QObject *sensors() const { return m_sensors; }
    QObject *perf() const { return m_perf; }
    QObject *system() const { return m_system; }
    bool available() const { return m_available; }

    void setEngine(QQmlEngine *engine);
    void setActive(bool on, bool dozing = false);

signals:
    void availableChanged();

private:
    QList<BusMirror *> mirrors() const { return {m_battery, m_charge, m_wifi, m_cable, m_cell, m_audio, m_bluetooth, m_sensors, m_perf}; }

    BusMirror *m_battery, *m_charge, *m_wifi, *m_cable, *m_cell, *m_audio, *m_bluetooth, *m_sensors, *m_perf;
    BusMirror *m_system;   // свойств нет — только вызовы: не слушает
    bool m_available = false;
};
