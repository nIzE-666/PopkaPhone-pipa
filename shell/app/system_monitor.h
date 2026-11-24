// НаомиФон — монитор системы на столе (docs/DESKTOP.md, «Монитор системы»): окно под индикатором подключения на панели
// (MonitorPopover.qml) — кадры стола и пропуски, процессор, видеочип, память и диск, тепло, Android; график за минуту.
// В QML — naomiMonitor и элемент MonitorGraph (Naomi.Desk).
//   Считает раз в секунду и только пока окно открыто (active): десяток маленьких файлов /proc и /sys, прав не нужно.
//   Закрыто — ни таймера, ни чтений, ни счёта кадров в столе.
//   - кадры: DesktopScreen::takePulse — кадров за секунду, кадр в потоке интерфейса (медиана и худший), пропуски
//     (опоздавший такт зрителя при сцене, ждущей кадра). Худший кадр и пропуски копятся между замерами — всплеск
//     внутри секунды не теряется;
//   - процессор: /proc/stat — все ядра и главное (последнее, cpu7 у Pixel 5);
//   - видеочип: kgsl gpubusy (занят / всего за последнее окно ядра, чтение его не сбрасывает) и gpuclk;
//   - память: MemTotal − MemAvailable; диск: /proc/vmstat pgpgin + pgpgout (счётчики блочных устройств на этом ядре
//     выключены — queue/iostats = 0);
//   - тепло: зоны skin-therm (корпус) и cpu-1-0-usr (процессор); «греется» — ядро срезало большим ядрам частоту
//     (scaling_max_freq ниже cpuinfo_max_freq);
//   - Android: группа процессов Waydroid (cgroup lxc.payload.waydroid) — cpu.stat, в процентах одного ядра, и
//     память программ (memory.stat: anon + shmem, без кэша файлов).
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariantMap>

#include <array>

class DesktopScreen;

class SystemMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // последний замер: fps, frameMs, worstMs, missed (за минуту), cpu, prime, gpu, gpuMhz, memGb, memTotalGb, diskMb,
    // skin, chip (°C), hot, android (% одного ядра), androidGb, hz, width, height
    Q_PROPERTY(QVariantMap now READ now NOTIFY sampled)
public:
    enum Lane { Frames, Cpu, Gpu, Disk, Temp, Android, Missed, LaneCount };
    Q_ENUM(Lane)
    static constexpr int kSeconds = 60;

    explicit SystemMonitor(QObject *parent = nullptr);
    void setDesk(DesktopScreen *desk) { m_desk = desk; }
    bool active() const { return m_timer.isActive(); }
    void setActive(bool on);
    QVariantMap now() const { return m_now; }
    int count() const { return m_count; }
    float at(int lane, int i) const { return m_hist[lane][(m_head + kSeconds - m_count + i) % kSeconds]; }   // 0 — старший

signals:
    void activeChanged();
    void sampled();

private:
    void sample(bool first);
    void push(const std::array<float, LaneCount> &v);

    DesktopScreen *m_desk = nullptr;
    QTimer m_timer;
    QElapsedTimer m_since;
    QVariantMap m_now;
    std::array<std::array<float, kSeconds>, LaneCount> m_hist{};
    int m_head = 0, m_count = 0;
    quint64 m_cpuBusy = 0, m_cpuAll = 0, m_primeBusy = 0, m_primeAll = 0;
    quint64 m_pages = 0, m_androidUs = 0;
    QString m_skinPath, m_chipPath;
    QStringList m_cpuZones;
};

// График одной величины за минуту: линия с заливкой, точка — сейчас; limit — черта порога (пунктир), marks — секунды
// с пропусками кадров (1 — бледно, 2 — ярко). Рисуется заново раз в секунду, по замеру монитора
class MonitorGraph : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(SystemMonitor *monitor READ monitor WRITE setMonitor NOTIFY changed)
    Q_PROPERTY(int lane MEMBER m_lane NOTIFY changed)
    Q_PROPERTY(double max MEMBER m_max NOTIFY changed)
    Q_PROPERTY(double min MEMBER m_min NOTIFY changed)
    Q_PROPERTY(double limit MEMBER m_limit NOTIFY changed)
    Q_PROPERTY(QColor color MEMBER m_color NOTIFY changed)
    Q_PROPERTY(int marks MEMBER m_marks NOTIFY changed)
public:
    explicit MonitorGraph(QQuickItem *parent = nullptr);
    SystemMonitor *monitor() const { return m_monitor; }
    void setMonitor(SystemMonitor *monitor);
    void paint(QPainter *painter) override;

signals:
    void changed();

private:
    QPointer<SystemMonitor> m_monitor;
    int m_lane = 0, m_marks = 0;
    double m_max = 100, m_min = 0, m_limit = -1;
    QColor m_color{Qt::white};
};
