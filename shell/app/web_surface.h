// НаомиФон — страница браузера в окне стола (docs/WEB.md; в QML — WebSurface, Naomi.Desk): ShellSurfaceItem, чья
// картинка обрезана по контуру окна — нижние углы страницы (и верхние, когда полосы нет) скруглены геометрией
// (outline_cut.h), а не слоем с маской: тот на каждый кадр страницы рисовал её во внеэкранный буфер и ещё раз —
// шейдером маски (ролик 1080p60: видеочип 75–82 % → 56–60 %, прогон 19 ноября 2026, docs/AUDIT.md, этап 1).
#pragma once

#include <QRectF>
#include <QWaylandQuickShellSurfaceItem>

class QWaylandSurface;

// Вложенный слой окна (subsurface) с кадром без прозрачности — у окон Wine содержимое окна лежит в таком слое.
// Qt Wayland сам завёл бы на слой обычный элемент (с прозрачностью); эти заводит WebSurface с opaque
class SolidSubsurface : public QWaylandQuickItem
{
    Q_OBJECT
public:
    explicit SolidSubsurface(QWaylandSurface *surface, QQuickItem *parent);
    // зовёт Qt Wayland (по имени), когда у слоя появляется свой вложенный
    Q_INVOKABLE void handleSubsurfaceAdded(QWaylandSurface *child);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    // слой лежит поверх WebSurface и нажатия по содержимому окна Wine получает сам: указатель — в точку нажатия
    void mousePressEvent(QMouseEvent *event) override;

private:
    bool m_fresh = true;   // пришёл новый кадр — текстуру грузить заново (solidNode)
};

// принимает от Qt Wayland вложенные слои элемента (QWaylandQuickItem::subsurfaceHandler) и заводит на них SolidSubsurface
class SolidSubsurfaces : public QObject
{
    Q_OBJECT
public:
    explicit SolidSubsurfaces(QQuickItem *item) : QObject(item), m_item(item) {}
    Q_INVOKABLE void handleSubsurfaceAdded(QWaylandSurface *child) { new SolidSubsurface(child, m_item); }

private:
    QQuickItem *m_item;
};

class WebSurface : public QWaylandQuickShellSurfaceItem
{
    Q_OBJECT
    // контур окна в единицах этого элемента (он уменьшен масштабом: точки стола × пикселей кадра в точке) и радиус углов
    Q_PROPERTY(QRectF outline READ outline WRITE setOutline NOTIFY outlineChanged)
    Q_PROPERTY(qreal corner READ corner WRITE setCorner NOTIFY outlineChanged)
    // пиксель кадра стола в единицах элемента — ширина каймы сглаживания края (0 — без неё)
    Q_PROPERTY(qreal pixel READ pixel WRITE setPixel NOTIFY outlineChanged)
    // Кадр непрозрачен, что бы ни стояло в старшем байте пикселя (окна Wine, docs/WINDOWS.md): Wine кладёт кадры
    // wl_shm формата XRGB с нулём в этом байте там, где рисовала сама программа, а Qt Wayland читает его как
    // прозрачность — окно выходило чёрным. Такой кадр стол берёт из памяти сам, грузит текстурой как есть и рисует
    // шейдером, который этот байт не читает (web_surface.cpp, solidNode; outline_cut.h, solid)
    Q_PROPERTY(bool opaque READ opaque WRITE setOpaque NOTIFY outlineChanged)
    // После колеса — ещё и wl_pointer.frame клиенту окна (окно Claude, docs/CLAUDE.md, «Колесо»). Chromium превращает
    // колесо в прокрутку только по frame (wayland_event_source.cc), а Qt объявляет wl_seat версии 4, где frame нет, и
    // сам его не шлёт: прокрутка копилась у Chromium и не доходила до страницы. Только окнам, которые этого просят:
    // клиент, у которого в разборе событий указателя нет frame, на чужом событии упал бы
    Q_PROPERTY(bool pointerFrames MEMBER m_pointerFrames)
    // Translate touch taps to clicks and swipes to wheel events for Linux desktop apps.
    Q_PROPERTY(bool touchToPointer READ touchToPointer WRITE setTouchToPointer NOTIFY touchToPointerChanged)
    Q_PROPERTY(int touchScrollSpeed MEMBER m_touchScrollSpeed)
public:
    explicit WebSurface(QQuickItem *parent = nullptr);
    QRectF outline() const { return m_outline; }
    qreal corner() const { return m_corner; }
    void setOutline(const QRectF &outline);
    void setCorner(qreal corner);
    qreal pixel() const { return m_pixel; }
    void setPixel(qreal pixel);
    bool opaque() const { return m_opaque; }
    void setOpaque(bool opaque);
    bool touchToPointer() const { return m_touchToPointer; }
    void setTouchToPointer(bool enabled);

signals:
    void outlineChanged();
    void touchToPointerChanged();
    void pointerPressed();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void sendPointerFrame(QInputEvent *event);
    void sendPointerFrameForSeat(QWaylandSeat *seat);
    void resetTouch();
    QRectF m_outline;
    qreal m_corner = 0;
    qreal m_pixel = 0;
    bool m_opaque = false;
    bool m_fresh = true;   // пришёл новый кадр — текстуру грузить заново (solidNode)
    bool m_pointerFrames = false;
    bool m_touchToPointer = false;
    int m_touchScrollSpeed = 100;
    int m_touchId = -1;
    QPointF m_touchStart;
    QPointF m_touchLast;
    QPointF m_touchScrollRemainder;
    bool m_touchScrolling = false;
};
