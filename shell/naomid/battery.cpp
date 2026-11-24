#include "battery.h"

#include <QDir>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const char UPOWER[] = "org.freedesktop.UPower";
const char DISPLAY_DEVICE[] = "/org/freedesktop/UPower/devices/DisplayDevice";
const char HISTORY[] = "/var/lib/upower";

qint64 sysNum(const QString &path)
{
    return readDeviceFile(path).toLongLong();
}

} // namespace

Battery::Battery(QObject *parent)
    : BusObject("/org/naomi/Phone/Battery", "org.naomi.Phone.Battery", parent)
    , m_thermal(DeviceProfile::current().thermalPaths())
{
    set("Percent", 0);
    set("State", "unknown");
    set("Charging", false);

    if (DeviceProfile::current().tablet) {
        // Начальное состояние до ответа UPower. Дальше DisplayDevice обновляет его по событиям.
        const QString battery = DeviceProfile::battery();
        bool ok = false;
        const int capacity = readDeviceFile(battery + "capacity").toInt(&ok);
        if (ok)
            set("Percent", qBound(0, capacity, 100));
        const auto status = readDeviceFile(battery + "status");
        uint state = status == "Charging" ? 1 : status == "Discharging" ? 2 : status == "Full" ? 4 : 0;
        if (status == "Not charging" && readDeviceFile(DeviceProfile::usbSupply() + "present") == "1")
            state = 5;
        if (state)
            apply({{"State", state}});
    }

    bus().connect(UPOWER, DISPLAY_DEVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                  SLOT(onUPower(QString,QVariantMap,QStringList)));
    follow(UPOWER, [this] {
        callAsync(UPOWER, DISPLAY_DEVICE, "org.freedesktop.DBus.Properties", "GetAll",
                  {"org.freedesktop.UPower.Device"}, [this](const QDBusMessage &reply) {
                      if (reply.type() == QDBusMessage::ReplyMessage)
                          apply(plainMap(reply.arguments().value(0)));
                  });
    });
}

void Battery::onUPower(const QString &, const QVariantMap &changed, const QStringList &)
{
    apply(plainMap(QVariant::fromValue(changed)));
}

void Battery::apply(const QVariantMap &upower)
{
    if (upower.contains("Percentage"))
        set("Percent", qRound(upower.value("Percentage").toDouble()));
    if (upower.contains("State")) {
        // UPower: 1 заряжается, 2 разряжается, 3 пуста, 4 полна, 5 ждёт зарядки, 6 ждёт разрядки
        static const char *const names[] = {"unknown", "charging", "discharging", "empty", "full", "plugged",
                                            "discharging"};
        const uint s = upower.value("State").toUInt();
        set("State", names[s < 7 ? s : 0]);
        const bool plugged = s == 1 || s == 4 || s == 5;
        // когда подключили или отключили: служба видела сама — точно; первое состояние после её запуска — не смена
        if (m_stateKnown && plugged != charging())
            m_plugChangedAt = QDateTime::currentSecsSinceEpoch();
        m_stateKnown = true;
        set("Charging", plugged);
    }
}

QVariantMap Battery::Sample() const
{
    const QString b = DeviceProfile::battery(), usb = DeviceProfile::usbSupply();
    return {
        {"currentMa", sysNum(b + "current_now") / 1000},
        {"currentAvgMa", sysNum(b + "current_avg") / 1000},   // среднее датчика — с него начинать сглаживание
        {"voltageMv", sysNum(b + "voltage_now") / 1000},
        {"batteryC", sysNum(b + "temp") / 10.0},
        {"cpuC", maxTemperature(m_thermal.cpu)},
        {"skinC", readDeviceFile(m_thermal.skin).toDouble() / 1000},
        // датчик заряда (fuel gauge): мкА·ч → мА·ч
        {"chargeMah", sysNum(b + "charge_counter") / 1000},
        {"fullMah", sysNum(b + "charge_full") / 1000},
        {"designMah", sysNum(b + "charge_full_design") / 1000},
        {"cycles", sysNum(b + "cycle_count")},
        {"toFullS", sysNum(b + "time_to_full_avg")},
        {"toEmptyS", sysNum(b + "time_to_empty_avg")},
        {"inputMa", sysNum(usb + "input_current_now") / 1000},
        {"inputMv", sysNum(usb + "voltage_now") / 1000},
    };
}

// Записи UPower: /var/lib/upower/history-<вид>-<Вт·ч по паспорту>-<серийный номер>.dat, строка — «время<TAB>значение
// <TAB>состояние». Вт·ч в имени UPower считает по напряжению при своём запуске, и после перезагрузки имя бывает
// другим (16, 17, 18) — история батареи лежит в нескольких файлах, их сливаем. «generic_id» — не батарея.
const QVector<Battery::Point> &Battery::records(const QString &kind, Cache &c) const
{
    const QFileInfoList files = QDir(HISTORY).entryInfoList({"history-" + kind + "-*.dat"}, QDir::Files, QDir::Name);
    QString stamp;
    QFileInfoList mine;
    for (const QFileInfo &f : files) {
        if (f.fileName().contains("generic_id"))
            continue;
        mine << f;
        stamp += f.fileName() + ':' + QString::number(f.size()) + ':'
                 + QString::number(f.lastModified().toSecsSinceEpoch()) + ';';
    }
    if (c.stamp == stamp)
        return c.points;
    c.stamp = stamp;
    c.points.clear();
    for (const QFileInfo &f : mine) {
        QFile file(f.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray all = file.readAll();
        for (const QByteArray &line : all.split('\n')) {
            const QList<QByteArray> p = line.split('\t');
            if (p.size() < 3)
                continue;
            const QByteArray s = p[2].trimmed();
            if (s == "unknown")   // метка перезапуска UPower: «0 unknown»
                continue;
            bool okT = false, okV = false;
            const qint64 t = p[0].toLongLong(&okT);
            const float v = p[1].toFloat(&okV);
            if (okT && okV)
                c.points.append({t, v, s == "charging" || s == "fully-charged" || s == "pending-charge",
                                 s == "discharging"});
        }
    }
    std::stable_sort(c.points.begin(), c.points.end(), [](const Point &a, const Point &b) { return a.t < b.t; });
    return c.points;
}

// Корзины по step = seconds / bins секунд, по границам, кратным step (сутки по 15 мин — по четвертям часа), последняя —
// с «сейчас». В каждой: level — заряд к концу корзины, low — наименьший за неё, plug — была подключена зарядка, watt —
// средняя мощность из записей UPower (в батарею +, из неё −; NaN — записей нет). Телефон выключен (записей в корзине
// нет) — level −1. После последней записи на диске (UPower сбрасывает их раз в 10 мин) — заряд держится прежним.
QVariantMap Battery::History(double seconds, double bins) const
{
    const int n = qBound(1, int(bins), 1000);
    const qint64 step = qMax<qint64>(60, qint64(seconds) / n);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 start = (now / step + 1) * step - step * n;
    const QVector<Point> &charge = records("charge", m_charge);
    const QVector<Point> &rate = records("rate", m_rate);
    const qint64 saved = qMax(charge.isEmpty() ? 0 : charge.last().t, rate.isEmpty() ? 0 : rate.last().t);
    const auto lower = [](const QVector<Point> &v, qint64 t) {
        return int(std::lower_bound(v.begin(), v.end(), t, [](const Point &p, qint64 x) { return p.t < x; })
                   - v.begin());
    };

    QVariantList level, low, plug, watt;
    for (int i = 0; i < n; ++i) {
        const qint64 b0 = start + i * step, b1 = qMin(b0 + step, now + 1);
        const bool current = i == n - 1;
        const int c0 = lower(charge, b0), c1 = lower(charge, b1), r0 = lower(rate, b0), r1 = lower(rate, b1);
        const bool alive = c1 > c0 || r1 > r0 || b0 >= saved || current;
        if (!alive || (c1 == 0 && !current)) {
            level << -1;
            low << -1;
            plug << 0;
            watt << std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        int lv = current ? percent() : qRound(charge[c1 - 1].v);
        int lo = c0 > 0 ? qRound(charge[c0 - 1].v) : lv;
        for (int k = c0; k < c1; ++k)
            lo = qMin(lo, qRound(charge[k].v));
        lo = qMin(lo, lv);
        // куда шла энергия: «discharging» — из батареи; при зарядке — по тому, куда шёл заряд (на доке ядро пишет
        // «Charging» и при разряде, когда док не успевает): за корзину, а не сдвинулся — по последнему сдвигу до неё
        int dir = c0 > 0 ? lv - qRound(charge[c0 - 1].v) : 0;
        for (int k = c1 - 1; dir == 0 && k > 0; --k)
            dir = qRound(charge[k].v) - qRound(charge[k - 1].v);
        bool pl = false, any = false;
        double sum = 0;
        for (int k = r0; k < r1; ++k) {
            any = true;
            pl |= rate[k].plugged;
            sum += rate[k].discharging || (rate[k].plugged && dir < 0) ? -rate[k].v : rate[k].v;
        }
        for (int k = c0; k < c1; ++k) {
            any = true;
            pl |= charge[k].plugged;
        }
        if (!any) {   // записей нет (хвост после сброса на диск): как было перед корзиной
            const int rk = lower(rate, b0) - 1;
            pl = rk >= 0 && rate[rk].plugged;
        }
        if (current)
            pl = pl || charging();
        level << lv;
        low << lo;
        plug << (pl ? 1 : 0);
        watt << (r1 > r0 ? sum / (r1 - r0) : std::numeric_limits<double>::quiet_NaN());
    }

    // когда в последний раз зарядили до 100 % и с каких пор нынешнее состояние (подключена или нет)
    qint64 lastFull = 0;
    for (int k = charge.size() - 1; k >= 0; --k)
        if (charge[k].plugged && charge[k].v >= 99.5) {
            lastFull = charge[k].t;
            break;
        }
    qint64 since = m_plugChangedAt;
    if (!since) {
        for (int k = rate.size() - 1; k >= 0; --k)
            if (rate[k].plugged != charging()) {
                since = k + 1 < rate.size() ? rate[k + 1].t : saved;
                break;
            }
    }
    return {
        {"start", start},
        {"step", step},
        {"level", level},
        {"low", low},
        {"plug", plug},
        {"watt", watt},
        {"lastFull", lastFull},
        {"since", since},
    };
}
