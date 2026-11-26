#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>

#include <array>

struct DeviceProfile
{
    struct DisplayMode { int hz; int configId; };
    struct CpuCluster { int firstCpu; int coreCount; int deskFloorKHz; };
    struct ThermalPaths { QStringList cpu; QString skin; };

    const char *name;
    bool tablet;
    bool pixel;
    double density;
    int panelDpi;
    int landscapeRotation;
    int defaultHz;
    QList<DisplayMode> displayModes;
    std::array<CpuCluster, 3> clusters;
    bool hasCellular = true;
    bool hasNfc = true;
    bool displayPowerOff = true;
    bool displayUbwc = true;
    int idleRefreshHz = 0;

    static const DeviceProfile &current();
    QString backlight() const;
    static QString battery();
    static QString usbSupply();
    ThermalPaths thermalPaths() const;
};

QByteArray readDeviceFile(const QString &path);
double maxTemperature(const QStringList &paths);
