#pragma once

#include "camera_engine.h"

#include <QImage>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QRectF>

#include <atomic>

class CameraViewfinder : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(CameraEngine *engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(int rotationDegrees READ rotationDegrees WRITE setRotationDegrees NOTIFY rotationDegreesChanged)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY mirroredChanged)
    Q_PROPERTY(bool fillCover READ fillCover WRITE setFillCover NOTIFY fillCoverChanged)

public:
    explicit CameraViewfinder(QQuickItem *parent = nullptr);

    CameraEngine *engine() const { return m_engine; }
    void setEngine(CameraEngine *engine);

    int rotationDegrees() const { return m_rotationDegrees; }
    void setRotationDegrees(int degrees);

    bool mirrored() const { return m_mirrored; }
    void setMirrored(bool mirrored);

    bool fillCover() const { return m_fillCover; }
    void setFillCover(bool fillCover);

    void paint(QPainter *painter) override;

    // Maps item-local tap coordinates back to normalized [0..1, 0..1] sensor coordinates.
    Q_INVOKABLE QPointF mapPointToSensor(qreal x, qreal y) const;

signals:
    void engineChanged();
    void rotationDegreesChanged();
    void mirroredChanged();
    void fillCoverChanged();

private slots:
    void onFrameReady();

private:
    CameraEngine *m_engine = nullptr;
    QImage m_frame;
    int m_rotationDegrees = 0;
    bool m_mirrored = false;
    bool m_fillCover = true;
    std::atomic<bool> m_updateQueued{false};
    QRectF m_lastDrawRect;
};
