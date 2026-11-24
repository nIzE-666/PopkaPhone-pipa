#include "web_surface.h"

#include "outline_cut.h"

#include <QImage>
#include <QInputEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTouchEvent>
#include <QWaylandBufferRef>
#include <QWaylandClient>
#include <QWaylandCompositor>
#include <QWaylandPointer>
#include <QWaylandSeat>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWheelEvent>
#include <QtWaylandCompositor/private/qwaylandpointer_p.h>
#include <wayland-server-protocol.h>

#include <algorithm>

WebSurface::WebSurface(QQuickItem *parent) : QWaylandQuickShellSurfaceItem(parent)
{
    // новый кадр поверхности — текстуру своего узла грузить заново (solidNode)
    connect(this, &QWaylandQuickItem::surfaceChanged, this, [this] {
        m_fresh = true;
        if (QWaylandSurface *s = surface())
            connect(s, &QWaylandSurface::redraw, this, [this] { m_fresh = true; }, Qt::UniqueConnection);
    });
}

void WebSurface::setOutline(const QRectF &outline)
{
    if (outline == m_outline)
        return;
    m_outline = outline;
    update();
    emit outlineChanged();
}

void WebSurface::setCorner(qreal corner)
{
    if (qFuzzyCompare(1 + corner, 1 + m_corner))
        return;
    m_corner = corner;
    update();
    emit outlineChanged();
}

void WebSurface::setPixel(qreal pixel)
{
    if (qFuzzyCompare(1 + pixel, 1 + m_pixel))
        return;
    m_pixel = pixel;
    update();
    emit outlineChanged();
}

// Кадр wl_shm элемента — своим узлом; рисует его cutToOutline(…, solid): старший байт пикселя за прозрачность не
// считается (у Wine в нём ноль — окно выходило чёрным). Байты кадра уходят в текстуру как есть, без копии: перевод
// каждого кадра в формат с прозрачностью стоил потоку оболочки половину большого ядра на игре 1560×721 (21 ноября 2026).
// Нет такого кадра — nullptr. oldNode — прошлый узел элемента: свой — обновляется, чужой — удаляется.
// fresh — с прошлой загрузки пришёл новый кадр (QWaylandSurface::redraw). Без него текстура остаётся прежней: узел
// перерисовывают и без нового кадра (контур окна, размер, сглаживание), и каждый раз кадр заново шёл в текстуру —
// у окна 1560×721 это 4,5 МБ через шину за раз; так же поступает и сам Qt Wayland (newTexture в updatePaintNode)
static QSGSimpleTextureNode *solidNode(QWaylandQuickItem *item, QSGNode *oldNode, bool &fresh)
{
    if (!item->view() || !item->window())
        return nullptr;
    const QWaylandBufferRef buffer = item->view()->currentBuffer();
    if (!buffer.hasBuffer() || !buffer.isSharedMemory())
        return nullptr;
    const QImage image = buffer.image();
    if (image.isNull())
        return nullptr;
    auto *own = dynamic_cast<QSGSimpleTextureNode *>(oldNode);
    if (!own || !own->ownsTexture()) {   // узел остался от Qt Wayland — свой
        delete oldNode;
        own = new QSGSimpleTextureNode;
        own->setOwnsTexture(true);
    }
    // Кадр — в текстуру на каждой перерисовке, как было. Пробовали грузить только по QWaylandSurface::redraw (fresh):
    // окно Telegram (wl_shm) замирало на первом кадре «Загрузка…» (23 ноября 2026) — откат до разбора
    own->setTexture(item->window()->createTextureFromImage(image));
    fresh = false;
    own->setFiltering(item->smooth() ? QSGTexture::Linear : QSGTexture::Nearest);
    own->setRect(QRectF(0, 0, item->width(), item->height()));
    // видимая часть кадра (wp_viewport: буфер у Wine больше окна)
    const QRectF source = item->surface() ? item->surface()->sourceGeometry() : QRectF();
    own->setSourceRect(source.isValid() ? source : QRectF(0, 0, image.width(), image.height()));
    return own;
}

SolidSubsurface::SolidSubsurface(QWaylandSurface *surface, QQuickItem *parent)
{
    setSubsurfaceHandler(this);
    setSurface(surface);
    setParentItem(parent);
    setParent(parent);
    // место слоя в родителе ведёт сам Qt Wayland (его закрытый слот)
    connect(surface, SIGNAL(subsurfacePositionChanged(QPoint)), this, SLOT(handleSubsurfacePosition(QPoint)));
    connect(surface, &QObject::destroyed, this, &QObject::deleteLater);
    connect(surface, &QWaylandSurface::redraw, this, [this] { m_fresh = true; });
}

void SolidSubsurface::handleSubsurfaceAdded(QWaylandSurface *child)
{
    new SolidSubsurface(child, this);
}

// Касание приходит нажатием мыши без движения перед ним: указатель Wayland остался там, где было прошлое касание,
// и нажатие попадало туда. Перед нажатием — указатель в точку нажатия
static void movePointerToPress(QWaylandQuickItem *item, QMouseEvent *event)
{
    if (!item->inputEventsEnabled() || !item->surface() || !item->view() || !item->compositor()
        || !item->inputRegionContains(event->position()))
        return;
    if (QWaylandSeat *seat = item->compositor()->seatFor(event))
        seat->sendMouseMoveEvent(item->view(), item->mapToSurface(event->position()), event->globalPosition());
}

void SolidSubsurface::mousePressEvent(QMouseEvent *event)
{
    movePointerToPress(this, event);
    QWaylandQuickItem::mousePressEvent(event);
}

QSGNode *SolidSubsurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    if (QSGSimpleTextureNode *node = solidNode(this, oldNode, m_fresh)) {
        cutToOutline(node, QRectF(), 0, QRectF(0, 0, width(), height()), 0, true);
        return node;
    }
    if (auto *own = dynamic_cast<QSGSimpleTextureNode *>(oldNode); own && own->ownsTexture()) {
        delete oldNode;
        oldNode = nullptr;
    }
    return QWaylandQuickItem::updatePaintNode(oldNode, data);
}

void WebSurface::setTouchToPointer(bool enabled)
{
    if (m_touchToPointer == enabled)
        return;
    m_touchToPointer = enabled;
    if (m_touchToPointer)
        setAcceptTouchEvents(true);
    else
        resetTouch();
    emit touchToPointerChanged();
}

void WebSurface::sendPointerFrameForSeat(QWaylandSeat *seat)
{
    if (!m_pointerFrames || !surface() || !surface()->client() || !seat || !seat->pointer())
        return;
    auto *pointer = static_cast<QWaylandPointerPrivate *>(QObjectPrivate::get(seat->pointer()));
    const auto resources = pointer->resourceMap().values(surface()->client()->client());
    for (QtWaylandServer::wl_pointer::Resource *resource : resources) {
        if (wl_resource_get_version(resource->handle) >= WL_POINTER_FRAME_SINCE_VERSION)
            pointer->send_frame(resource->handle);
    }
}

void WebSurface::sendPointerFrame(QInputEvent *event)
{
    if (!m_pointerFrames || !event->isAccepted() || !compositor())
        return;
    sendPointerFrameForSeat(compositor()->seatFor(event));
}

void WebSurface::mousePressEvent(QMouseEvent *event)
{
    emit pointerPressed();
    movePointerToPress(this, event);
    QWaylandQuickShellSurfaceItem::mousePressEvent(event);
    sendPointerFrame(event);
}

void WebSurface::mouseMoveEvent(QMouseEvent *event)
{
    QWaylandQuickShellSurfaceItem::mouseMoveEvent(event);
    sendPointerFrame(event);
}

void WebSurface::mouseReleaseEvent(QMouseEvent *event)
{
    QWaylandQuickShellSurfaceItem::mouseReleaseEvent(event);
    sendPointerFrame(event);
}

void WebSurface::touchEvent(QTouchEvent *event)
{
    if (!m_touchToPointer) {
        QWaylandQuickShellSurfaceItem::touchEvent(event);
        return;
    }
    const auto type = event->type();
    if (type == QEvent::TouchCancel) {
        resetTouch();
        event->accept();
        return;
    }
    if (!inputEventsEnabled() || !surface() || !view() || !compositor()) {
        resetTouch();
        event->ignore();
        return;
    }
    QWaylandSeat *seat = compositor()->seatFor(event);
    if (!seat) {
        resetTouch();
        event->ignore();
        return;
    }

    const auto &points = event->points();
    if (type == QEvent::TouchBegin) {
        resetTouch();
        if (points.isEmpty() || !inputRegionContains(points.constFirst().position())) {
            event->ignore();
            return;
        }
        const auto &primary = points.constFirst();
        m_touchId = primary.id();
        m_touchStart = m_touchLast = primary.position();
        seat->sendMouseMoveEvent(view(), mapToSurface(m_touchStart), mapToScene(m_touchStart));
        sendPointerFrameForSeat(seat);
        event->accept();
        return;
    }

    if (m_touchId < 0) {
        event->ignore();
        return;
    }
    event->accept();
    const auto primary = std::find_if(points.cbegin(), points.cend(), [this](const QEventPoint &point) {
        return point.id() == m_touchId;
    });
    if (primary == points.cend()) {
        if (type == QEvent::TouchEnd)
            resetTouch();
        return;
    }

    // Порог «это уже не касание, а прокрутка» — в точках стола (сцены), как touch slop у Android (8 dp). В единицах
    // элемента он зависел от растяжения окна (GameWindow: Scale sx/sy) и масштаба стола: у развёрнутого окна и в
    // доке палец проходил разное расстояние до прокрутки
    constexpr qreal tapThreshold = 8;
    const QPointF pos = primary->position();
    const QPointF fromStart = mapToScene(pos) - mapToScene(m_touchStart);
    m_touchScrolling |= QPointF::dotProduct(fromStart, fromStart) > tapThreshold * tapThreshold;

    if (type == QEvent::TouchEnd || primary->state() == QEventPoint::State::Released) {
        if (!m_touchScrolling) {
            emit pointerPressed();
            if (focusOnClick())
                takeFocus(seat);
            seat->sendMouseMoveEvent(view(), mapToSurface(m_touchStart), mapToScene(m_touchStart));
            seat->sendMousePressEvent(Qt::LeftButton);
            sendPointerFrameForSeat(seat);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            sendPointerFrameForSeat(seat);
        }
        resetTouch();
        return;
    }

    const QPointF step = pos - m_touchLast;
    m_touchLast = pos;
    if (!m_touchScrolling || step.isNull())
        return;

    // Qt Wayland uses whole axis units. Carry fractions so slow swipes still scroll.
    const qreal speed = qBound(10, m_touchScrollSpeed, 100) / 100.0;
    const QPointF scaledStep = step * speed + m_touchScrollRemainder;
    const QPoint delta = scaledStep.toPoint();
    m_touchScrollRemainder = scaledStep - QPointF(delta);
    if (delta.isNull())
        return;

    // Keep the wheel over the control where the gesture started.
    if (seat->mouseFocus() != view())
        seat->sendMouseMoveEvent(view(), mapToSurface(m_touchStart), mapToScene(m_touchStart));
    // Колесо — прямо в seat, без синтетического QWheelEvent: тот переписывал «точку 0» своего устройства — сначала
    // тачскрина (портил отпускание пальца), после перехода на устройство по умолчанию — мыши. Qt Wayland шлёт ось
    // -delta / 12 — со шагом delta * 12 это прежние знаки и величины: свайп вверх или влево — положительная ось
    if (delta.x())
        seat->sendMouseWheelEvent(Qt::Horizontal, delta.x() * 12);
    if (delta.y())
        seat->sendMouseWheelEvent(Qt::Vertical, delta.y() * 12);
    sendPointerFrameForSeat(seat);
}

void WebSurface::resetTouch()
{
    m_touchId = -1;
    m_touchScrolling = false;
    m_touchScrollRemainder = {};
}

void WebSurface::touchUngrabEvent()
{
    resetTouch();
    QWaylandQuickShellSurfaceItem::touchUngrabEvent();
}

void WebSurface::wheelEvent(QWheelEvent *event)
{
    QWaylandQuickShellSurfaceItem::wheelEvent(event);   // wl_pointer.axis — Qt
    sendPointerFrame(event);
}

void WebSurface::setOpaque(bool opaque)
{
    if (opaque == m_opaque)
        return;
    m_opaque = opaque;
    // вложенные слои окна — тоже без прозрачности; задаётся до того, как у элемента появится поверхность
    setSubsurfaceHandler(opaque ? new SolidSubsurfaces(this) : nullptr);
    update();
    emit outlineChanged();
}

QSGNode *WebSurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    if (m_opaque) {
        if (QSGSimpleTextureNode *own = solidNode(this, oldNode, m_fresh)) {
            cutToOutline(own, m_outline, m_corner, QRectF(0, 0, width(), height()), m_pixel, true);
            return own;
        }
    }
    if (auto *own = dynamic_cast<QSGSimpleTextureNode *>(oldNode); own && own->ownsTexture()) {
        delete oldNode;   // был свой узел — Qt Wayland пусть заведёт свой
        oldNode = nullptr;
    }
    QSGNode *node = QWaylandQuickShellSurfaceItem::updatePaintNode(oldNode, data);
    cutToOutline(dynamic_cast<QSGSimpleTextureNode *>(node), m_outline, m_corner, QRectF(0, 0, width(), height()),
                 m_pixel);
    return node;
}
