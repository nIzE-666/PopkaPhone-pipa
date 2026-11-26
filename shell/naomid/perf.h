// naomid — управление производительностью, профилями питания, термозащитой корпуса и сном.
// - На Pixel 5 (pixel): «стол на полную» (HoldDesk) для трансляции стола.
// - На планшетах (pipa и другие ARM64/SM8250):
//   * Многоуровневые профили eco / balanced / performance (автообнаружение кластеров policy* и GPU kgsl-3d0,
//     настроенный ondemand + потолки частот + input_boost + force_no_nap=0).
//   * Мягкая ступенчатая термозащита по датчику корпуса (quiet_therm / skin-therm: 39 / 42 / 45 °C, гистерезис 2 °C).
//   * Управляемый цикл сна: динамический wake_lock (держится при включённом экране, на кабеле или при stayAwake),
//     синхронизация screen_state и опциональный глубокий сон (mem) от батареи при выключенном экране.
#pragma once

#include "bus_object.h"

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QFile>
#include <QList>
#include <QSet>
#include <QTimer>

class Gpu;

class Perf : public BusObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.Perf")
    Q_PROPERTY(bool Desk READ desk)
    Q_PROPERTY(QString Profile READ profile)
    Q_PROPERTY(bool StayAwake READ stayAwake)
    Q_PROPERTY(bool DeepSuspend READ deepSuspend)
    Q_PROPERTY(int ThermalLevel READ thermalLevel)
    Q_PROPERTY(double SkinTempC READ skinTempC)

public:
    explicit Perf(Gpu *gpu, QObject *parent = nullptr);
    ~Perf() override;
    bool desk() const { return prop("Desk").toBool(); }
    QString profile() const { return prop("Profile").toString(); }
    bool stayAwake() const { return prop("StayAwake").toBool(); }
    bool deepSuspend() const { return prop("DeepSuspend").toBool(); }
    int thermalLevel() const { return prop("ThermalLevel").toInt(); }
    double skinTempC() const { return prop("SkinTempC").toDouble(); }

public slots:
    Q_SCRIPTABLE void HoldDesk(bool on);
    Q_SCRIPTABLE void SetProfile(const QString &profile);
    Q_SCRIPTABLE void SetStayAwake(bool on);
    Q_SCRIPTABLE void SetDeepSuspend(bool on);
    Q_SCRIPTABLE void SetDisplayOn(bool on);

private:
    struct PolicyInfo {
        QString dir;
        int firstCpu = 0;
        int minKHz = 0;
        int maxKHz = 0;
        QList<int> freqs;
    };

    void apply();
    void floors(bool on);
    void awake(bool on);
    void boostShell(bool on);
    void boostApps(bool on);
    void skin(bool on);

    // Планшетный универсальный контроллер (SM8250 / ARM64)
    void discoverTopology();
    void applyTabletProfile();
    void pollThermal();
    void updateWakeLock();
    void tryDeepSuspend();
    void saveTabletState() const;
    int pickCeilingKHz(const PolicyInfo &p, double ratio) const;

    Gpu *m_gpu;
    QSet<QString> m_holders;
    QDBusServiceWatcher m_watch;
    uint m_shellPid = 0;
    QFile m_cpuLatency;

    QList<PolicyInfo> m_policies;
    QString m_skinThermalPath;
    int m_gpuLevels = 0;
    bool m_displayOn = true;
    bool m_wakeLockHeld = false;
    QTimer m_thermalTimer;
    QTimer m_suspendTimer;
};
