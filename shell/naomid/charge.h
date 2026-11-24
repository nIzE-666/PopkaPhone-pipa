// naomid — заряд батареи (docs/NAOMID.md, «Заряд»): держит заряд в коридоре по режиму через пороги ядра Google
// (google_charger: charge_stop_level — «заряжать до», charge_start_level — «снова заряжать ниже»); жара, калибровка
// процентов, новая батарея, «до 100 % один раз», выключение на 3 %, запасная защита ядра от жары. Пороги ядро после
// загрузки сбрасывает в 100 / 0 — служба ставит их сама при запуске и смотрит раз в 30 с (и сразу, когда UPower
// сообщает о зарядке). Настройки — /userdata/naomi/state/charge.conf.
#pragma once

#include "bus_object.h"

#include <QTimer>

class Charge : public BusObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.Charge")
    Q_PROPERTY(QString Mode READ mode)        // daily (75–80 %), dock (45–55 %), custom (Upper/Lower), off (до 100 %)
    Q_PROPERTY(int Upper READ upper)          // «Своё»: заряжать до
    Q_PROPERTY(int Lower READ lower)          // «Своё»: снова заряжать ниже
    Q_PROPERTY(bool Heat READ heat)           // жара: 35 °C полчаса на кабеле — верх 60 %, 40 °C — пауза до 37 °C
    Q_PROPERTY(bool Defender READ defender)   // запасная защита ядра от жары (TEMP-DEFEND), когда предела нет
    Q_PROPERTY(bool Calibrate READ calibrate) // калибровка процентов раз в 30 дней
    Q_PROPERTY(bool LowOff READ lowOff)       // выключить телефон на 3 % без кабеля
    Q_PROPERTY(bool FullOnce READ fullOnce)   // идёт «до 100 % один раз»
    // battery (без кабеля), charging, holding (держит: заряд стоит, питание от кабеля), short (на кабеле, а заряд уходит:
    // кабель не покрывает расход), draining (выше верха — спускается от батареи), hot (пауза: жарко), full («один
    // раз»), calibrating (калибровка), learning (новая батарея: первый полный заряд)
    Q_PROPERTY(QString State READ state)
    Q_PROPERTY(bool Warm READ warm)           // жарко полчаса — верх снижен до 60 %
    Q_PROPERTY(int Stop READ stop)            // пороги в ядре сейчас: заряжать до…
    Q_PROPERTY(int Start READ start)          // …и снова ниже
    Q_PROPERTY(qlonglong LastFull READ lastFull)            // когда последний раз был 100 % (с эпохи), 0 — не видела
    Q_PROPERTY(qlonglong NextCalibration READ nextCalibration)   // когда калибровка (0 — выключена или не нужна)
    // чем держится предел: google (пороги google_charger, Pixel), night (night_charging ядра Xiaomi, Pad 6 — стоп заряда
    // только от 80 %), none (нечем — режим off)
    Q_PROPERTY(QString Backend READ backend)
    Q_PROPERTY(int MinUpper READ minUpper)    // ниже этого верха держать нечем (night — 80, google — 50)

public:
    explicit Charge(QObject *parent = nullptr);
    ~Charge() override;

    QString mode() const { return prop("Mode").toString(); }
    int upper() const { return prop("Upper").toInt(); }
    int lower() const { return prop("Lower").toInt(); }
    bool heat() const { return prop("Heat").toBool(); }
    bool defender() const { return prop("Defender").toBool(); }
    bool calibrate() const { return prop("Calibrate").toBool(); }
    bool lowOff() const { return prop("LowOff").toBool(); }
    bool fullOnce() const { return prop("FullOnce").toBool(); }
    QString state() const { return prop("State").toString(); }
    bool warm() const { return prop("Warm").toBool(); }
    int stop() const { return prop("Stop").toInt(); }
    int start() const { return prop("Start").toInt(); }
    qlonglong lastFull() const { return prop("LastFull").toLongLong(); }
    qlonglong nextCalibration() const { return prop("NextCalibration").toLongLong(); }
    QString backend() const { return prop("Backend").toString(); }
    int minUpper() const { return prop("MinUpper").toInt(); }

public slots:
    Q_SCRIPTABLE void SetMode(const QString &mode);
    // «Своё»: верх 50…100, низ 20…верх − 5 (режим — custom)
    Q_SCRIPTABLE void SetLimits(int upper, int lower);
    // heat, defender, calibrate, lowoff
    Q_SCRIPTABLE void SetOption(const QString &name, bool on);
    // «до 100 % один раз»: до отключения кабеля или 3 ч на 100 %; false — отменить
    Q_SCRIPTABLE void ChargeFull(bool on);
    // здоровье батареи: fullMah, designMah, cycles, resistanceMohm, ocvMv, tempC, swelling (сколько батарея провела
    // тёплой и почти полной — её собственный счётчик: [{tempC, soc, chargeS, dischargeS}]), since (с какого дня служба
    // видит эту батарею), lastFull, nextCalibration
    Q_SCRIPTABLE QVariantMap Health() const;

private slots:
    void onUPower(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    enum class Cal { None, Drain, Charge, Hold };   // калибровка: спуститься до 15 % → до 100 % → полчаса на 100 %

    void tick();
    void save();
    void setCal(Cal phase, bool isNew = false);
    bool writeLevels(int stop, int start, int level);
    bool writeNight(bool hold);
    void applyDefender();
    bool google() const { return backend() == QLatin1String("google"); }
    bool night() const { return backend() == QLatin1String("night"); }
    bool limited() const { return google() || night(); }

    bool m_nightHold = false;        // night: заряд остановлен (night_charging = 1)
    int m_stopNow = 100, m_startNow = 0;   // night: пороги, которые держит служба (в ядре их нет)

    QTimer m_timer, m_soon;
    qint64 m_lastFull = 0, m_lastCal = 0, m_serialSince = 0;
    QString m_serial;
    Cal m_cal = Cal::None;
    bool m_calNew = false;           // калибровка — из-за новой батареи
    qint64 m_calSince = 0;           // фаза Hold — с каких пор на 100 %
    bool m_fullSawPlug = false;      // «один раз»: кабель был — отключение заканчивает
    qint64 m_fullReachedAt = 0;      // «один раз»: когда дошёл до 100 %
    bool m_hot = false;
    qint64 m_warmSince = 0;          // теплее 35 °C на кабеле — с каких пор
    bool m_plugged = false;
    int m_level = -1;
    int m_lowTicks = 0;              // подряд на 3 % и ниже без кабеля
    int m_overTicks = 0;             // подряд напряжение покоя выше, чем бывает на верхе
    QString m_serialCandidate;       // другой серийный номер — сколько проходов подряд
    int m_serialSeen = 0;
};
