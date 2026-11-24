// naomid — служба железа НаомиФона (docs/NAOMID.md): одна точка, через которую оболочка и программы
// узнают о железе и управляют им. Системная служба (root), имя org.naomi.Phone на системной шине;
// звать её могут только phablet и root (/etc/dbus-1/system.d/org.naomi.Phone.conf). Сама ничего не
// опрашивает: слушает NetworkManager, UPower, oFono, BlueZ, PulseAudio, sensorfwd и пересылает изменения.
#include "android.h"
#include "audio.h"
#include "battery.h"
#include "bluetooth.h"
#include "cable.h"
#include "cell.h"
#include "charge.h"
#include "device_profile.h"
#include "gpu.h"
#include "net.h"
#include "perf.h"
#include "sensors.h"
#include "system.h"
#include "unix_signals.h"
#include "wifi.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QLoggingCategory>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("naomid");
    const auto &profile = DeviceProfile::current();
    if (app.arguments().size() == 2) {
        const QString option = app.arguments().at(1);
        if (option == "--device" || option == "--backlight") {
            QTextStream(stdout) << (option == "--device" ? QString::fromLatin1(profile.name) : profile.backlight()) << '\n';
            return 0;
        }
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCritical("naomid: нет системной шины: %s", qPrintable(bus.lastError().message()));
        return 1;
    }
    Battery battery;
    Charge charge;
    Wifi wifi;
    Cell cell;
    Audio audio;
    Bluetooth bluetooth;
    Sensors sensors;
    Net net;
    Cable cable;
    Gpu gpu;
    Perf perf(&gpu);
    System system;
    Android android;
    // сначала объекты, потом имя: клиент, увидевший имя, сразу находит всё
    for (BusObject *o :
         std::initializer_list<BusObject *>{&battery, &charge, &wifi, &cell, &audio, &bluetooth, &sensors, &net, &cable, &gpu,
                                             &perf, &system, &android})
        if (!o->publish())
            return 1;
    if (!bus.registerService("org.naomi.Phone")) {
        qCritical("naomid: не занять имя org.naomi.Phone: %s", qPrintable(bus.lastError().message()));
        return 1;
    }
    auto *unixSignals = new UnixSignals(&app);
    QObject::connect(unixSignals, &UnixSignals::terminate, &app, &QCoreApplication::quit);
    qInfo("naomid: профиль %s, кластеры CPU %d+%d+%d", profile.name, profile.clusters[0].coreCount,
          profile.clusters[1].coreCount, profile.clusters[2].coreCount);
    return app.exec();
}
