// naomid — батарея: заряд и зарядка от UPower (по событиям), мгновенные ток, напряжение, температуры, ёмкость и
// циклы — по запросу Sample() (для HUD, «о телефоне» и окна батареи стола; сами по себе не опрашиваются), история
// заряда — History() из записей UPower.
#pragma once

#include "bus_object.h"
#include "device_profile.h"

#include <QDateTime>
#include <QVector>

class Battery : public BusObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.Battery")
    Q_PROPERTY(int Percent READ percent)
    Q_PROPERTY(QString State READ state)      // charging, discharging, full, plugged (подключён, не заряжается), empty, unknown
    Q_PROPERTY(bool Charging READ charging)   // зарядка подключена: charging, full, plugged

public:
    explicit Battery(QObject *parent = nullptr);

    int percent() const { return prop("Percent").toInt(); }
    QString state() const { return prop("State").toString(); }
    bool charging() const { return prop("Charging").toBool(); }

public slots:
    // currentMa (+ заряд, − разряд), currentAvgMa (среднее датчика), voltageMv, batteryC, cpuC, skinC; датчик заряда: chargeMah (запас), fullMah
    // (ёмкость сейчас), designMah (по паспорту), cycles, toFullS / toEmptyS (оценка ядра, −1 — нет); вход зарядки
    // (USB): inputMa, inputMv
    Q_SCRIPTABLE QVariantMap Sample() const;
    // уровень заряда за последние seconds, по bins корзинам (docs/NAOMID.md, «История заряда»)
    Q_SCRIPTABLE QVariantMap History(double seconds, double bins) const;

private slots:
    void onUPower(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void apply(const QVariantMap &upower);

    struct Point
    {
        qint64 t;
        float v;
        bool plugged;      // зарядка подключена: charging, fully-charged, pending-charge
        bool discharging;
    };

    DeviceProfile::ThermalPaths m_thermal;
    bool m_stateKnown = false;
    qint64 m_plugChangedAt = 0;   // когда подключили или отключили зарядку (служба видела сама); 0 — не видела
    struct Cache
    {
        QString stamp;     // имена, размеры и время правки файлов
        QVector<Point> points;
    };
    // записи UPower одного вида (charge, rate) из всех файлов батареи, по времени; перечитываются, только если файлы
    // сменились
    const QVector<Point> &records(const QString &kind, Cache &cache) const;
    mutable Cache m_charge, m_rate;
};
