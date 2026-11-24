#include "camera_viewfinder.h"

#include <QPainter>
#include <QTransform>

#include <algorithm>

CameraViewfinder::CameraViewfinder(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setRenderTarget(QQuickPaintedItem::Image);
    setOpaquePainting(true);
    setAntialiasing(false);
}

void CameraViewfinder::setEngine(CameraEngine *engine)
{
    if (m_engine == engine)
        return;
    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);
    m_engine = engine;
    if (m_engine) {
        connect(m_engine, &CameraEngine::frameReady, this, &CameraViewfinder::onFrameReady, Qt::QueuedConnection);
        m_frame = m_engine->latestFrame();
    }
    emit engineChanged();
    update();
}

void CameraViewfinder::setRotationDegrees(int degrees)
{
    const int norm = ((degrees % 360) + 360) % 360;
    if (m_rotationDegrees == norm)
        return;
    m_rotationDegrees = norm;
    emit rotationDegreesChanged();
    update();
}

void CameraViewfinder::setMirrored(bool mirrored)
{
    if (m_mirrored == mirrored)
        return;
    m_mirrored = mirrored;
    emit mirroredChanged();
    update();
}

void CameraViewfinder::setFillCover(bool fillCover)
{
    if (m_fillCover == fillCover)
        return;
    m_fillCover = fillCover;
    emit fillCoverChanged();
    update();
}

void CameraViewfinder::onFrameReady()
{
    if (!m_engine)
        return;
    m_frame = m_engine->latestFrame();
    if (!m_updateQueued.exchange(true)) {
        update();
    }
}

void CameraViewfinder::paint(QPainter *painter)
{
    m_updateQueued.store(false);

    const QRectF bounds = boundingRect();
    painter->fillRect(bounds, QColor(QStringLiteral("#0e0f0c")));

    if (m_frame.isNull() || bounds.width() <= 1.0 || bounds.height() <= 1.0)
        return;

    const bool quarterTurn = (m_rotationDegrees == 90 || m_rotationDegrees == 270);
    const qreal effW = quarterTurn ? static_cast<qreal>(m_frame.height()) : static_cast<qreal>(m_frame.width());
    const qreal effH = quarterTurn ? static_cast<qreal>(m_frame.width()) : static_cast<qreal>(m_frame.height());
    if (effW <= 0.0 || effH <= 0.0)
        return;

    const qreal scaleX = bounds.width() / effW;
    const qreal scaleY = bounds.height() / effH;
    const qreal scale = m_fillCover ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);

    const qreal drawW = effW * scale;
    const qreal drawH = effH * scale;
    m_lastDrawRect = QRectF((bounds.width() - drawW) * 0.5,
                            (bounds.height() - drawH) * 0.5,
                            drawW,
                            drawH);

    painter->save();
    painter->setClipRect(bounds);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->translate(bounds.center());
    if (m_mirrored)
        painter->scale(-1.0, 1.0);
    if (m_rotationDegrees != 0)
        painter->rotate(static_cast<qreal>(m_rotationDegrees));

    const qreal srcDrawW = static_cast<qreal>(m_frame.width()) * scale;
    const qreal srcDrawH = static_cast<qreal>(m_frame.height()) * scale;
    const QRectF target(-srcDrawW * 0.5, -srcDrawH * 0.5, srcDrawW, srcDrawH);
    painter->drawImage(target, m_frame);
    painter->restore();
}

QPointF CameraViewfinder::mapPointToSensor(qreal x, qreal y) const
{
    const QRectF ref = m_lastDrawRect.isValid() && m_lastDrawRect.width() > 1.0 ? m_lastDrawRect : boundingRect();
    if (ref.width() <= 1.0 || ref.height() <= 1.0)
        return QPointF(0.5, 0.5);

    qreal nx = std::clamp((x - ref.left()) / ref.width(), 0.0, 1.0);
    qreal ny = std::clamp((y - ref.top()) / ref.height(), 0.0, 1.0);

    if (m_mirrored)
        nx = 1.0 - nx;

    switch (m_rotationDegrees) {
    case 90:
        return QPointF(ny, 1.0 - nx);
    case 180:
        return QPointF(1.0 - nx, 1.0 - ny);
    case 270:
        return QPointF(1.0 - ny, nx);
    default:
        return QPointF(nx, ny);
    }
}
