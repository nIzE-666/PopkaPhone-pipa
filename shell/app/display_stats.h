// НаомиФон — цифры экрана для QML: реально выведенные кадры и частота панели (от eglfs_naomi), ширина кадра.
#pragma once

#include "naomi_display.h"

#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVariantList>

// служба naomi-shell перезапускает оболочку с этим кодом выхода (RestartForceExitStatus)
inline constexpr int kRestartExitCode = 75;

class DisplayStats : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double refreshHz READ refreshHz NOTIFY changed)
    Q_PROPERTY(double fps READ fps NOTIFY changed)
    Q_PROPERTY(double medianMs READ medianMs NOTIFY changed)
    Q_PROPERTY(double p95Ms READ p95Ms NOTIFY changed)
    Q_PROPERTY(double maxMs READ maxMs NOTIFY changed)
    Q_PROPERTY(qulonglong frames READ frames NOTIFY changed)
    Q_PROPERTY(qulonglong drops READ drops NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QVariantList refreshRates READ refreshRates CONSTANT)
    // кадр, который рисует Qt, в пикселях: меньше панели — его растягивает дисплей (docs/DISPLAY.md)
    Q_PROPERTY(QSize renderSize READ renderSize CONSTANT)
    Q_PROPERTY(QSize panelSize READ panelSize CONSTANT)
    Q_PROPERTY(int sceneRotation READ sceneRotation WRITE setSceneRotation NOTIFY sceneRotationChanged)
    Q_PROPERTY(bool autoRotate READ autoRotate WRITE setAutoRotate NOTIFY autoRotateChanged)

public:
    explicit DisplayStats(QObject *parent = nullptr);

    double refreshHz() const { return m_s.refreshHz; }
    double fps() const { return m_s.fps; }
    double medianMs() const { return m_s.medianMs; }
    double p95Ms() const { return m_s.p95Ms; }
    double maxMs() const { return m_s.maxMs; }
    qulonglong frames() const { return m_s.frames; }
    qulonglong drops() const { return m_s.drops; }
    bool available() const { return m_snapshot != nullptr; }
    QSize renderSize() const;
    QSize panelSize() const;
    int sceneRotation() const { return m_sceneRotation; }
    bool autoRotate() const { return m_autoRotate; }
    QVariantList refreshRates() const;

    void setSceneRotation(int rotation);
    void setAutoRotate(bool enabled);
    Q_INVOKABLE void chooseOrientation(int rotation);
    Q_INVOKABLE void toggleOrientation();
    Q_INVOKABLE bool setRefresh(int hz);
    // ширина кадра (1080 — во всю панель, 720 — 720 × 1560): запомнить и перезапустить оболочку
    Q_INVOKABLE void setRenderWidth(int width);
    void setActive(bool on);   // экран выключен — не опрашивать (иначе интерфейс перерисовывается)

    // выбор из «Настроек» (display/render в shell.conf); до QGuiApplication — его читает main()
    static int savedRenderWidth();

signals:
    void changed();
    void sceneRotationChanged();
    void autoRotateChanged();
    void restartRequested();

private:
    int m_sceneRotation = 0;
    bool m_autoRotate = false;
    NaomiFrameSnapshotFn m_snapshot = nullptr;
    NaomiSetRefreshFn m_setRefresh = nullptr;
    NaomiFrameSnapshot m_s{};
    QTimer m_timer;
};
