#include "desktop_screen.h"
#include "scene_rotation.h"
#include "naomi_display.h"

#include <QAbstractAnimation>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QImage>
#include <QTransform>
#include <QLoggingCategory>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlIncubationController>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QWaylandCompositor>
#include <QWaylandOutputMode>
#include <QWaylandQuickOutput>
#include <QtWaylandCompositor/private/qwaylandpresentationtime_p.h>
#include <private/qqmldata_p.h>        // трасса: где в QML элемент (traceScene)
#include <private/qquickitem_p.h>      // трасса: что в сцене сменилось к кадру
#include <private/qquickwindow_p.h>
#include <private/qsgrenderloop_p.h>   // часы анимаций Qt — у его цикла отрисовки (setClock)

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

Q_LOGGING_CATEGORY(lcDesktop, "naomi.desktop")

namespace {

qint64 steadyUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// NAOMI_DESK_TRACE=файл — ход кадров стола для разбора плавности (docs/PI.md, «Анимации и след прозрачного»): строки
// «t,мкс,просила ли сцена» — такт зрителя, «q,мкс,0» — сцена впервые после кадра просит новый, «r,мкс,polish,sync,render,end» —
// кадр и сколько заняли его части (мкс, steady_clock; render — с упаковкой для кодера в afterRendering);
// «d,мкс,элементы» — что в сцене сменилось к этому кадру и «p,мкс,элементы» — что ждёт polish (traceScene).
// Включить и выключить на ходу, без перезапуска оболочки, — DesktopScreen::setTrace (зонд стола, desk_probe.h)
FILE *g_trace = [] {
    const QByteArray p = qgetenv("NAOMI_DESK_TRACE");
    return p.isEmpty() ? nullptr : fopen(p.constData(), "w");
}();

bool g_traceScene = true;   // строки d и p (что сменилось, что ждёт polish) — с ними кадр дороже на ~0,1–0,3 мс

FILE *deskTrace()
{
    return g_trace;
}

// Элемент сцены для трассы: тип QML, id (или «<id» ближайшего предка с id), файл и строка — «Text<tile@FilesWindow.qml:957»
QByteArray itemName(QQuickItem *item)
{
    QByteArray cls = item->metaObject()->className();
    if (const int q = cls.indexOf("_QML"); q > 0)
        cls.truncate(q);
    if (cls.startsWith("QQuick"))
        cls.remove(0, 6);
    QByteArray name = cls;
    for (QQuickItem *p = item; p; p = p->parentItem()) {
        const QQmlContext *c = qmlContext(p);
        const QString id = c ? c->nameForObject(p) : QString();
        if (!id.isEmpty()) {
            name += (p == item ? "#" : "<") + id.toUtf8();
            break;
        }
    }
    if (const QQmlData *d = QQmlData::get(item); d && d->outerContext)
        name += "@" + d->outerContext->url().fileName().toUtf8() + ":" + QByteArray::number(d->lineNumber);
    return name;
}

// Что сменилось в сцене к кадру (грязные элементы, флаги QQuickItemPrivate::DirtyType) и что ждёт polish: кто
// заставляет стол рисовать, когда на вид ничего не движется. Не больше 12 штук на строку.
void traceScene(FILE *f, QQuickWindow *window, qint64 us)
{
    QQuickWindowPrivate *w = QQuickWindowPrivate::get(window);
    QByteArray d, p;
    int n = 0;
    for (QQuickItem *i = w->dirtyItemList; i && n < 12; i = QQuickItemPrivate::get(i)->nextDirtyItem, ++n)
        d += itemName(i) + "=" + QByteArray::number(QQuickItemPrivate::get(i)->dirtyAttributes, 16) + "("
           + QByteArray::number(i->x(), 'f', 1) + " " + QByteArray::number(i->y(), 'f', 1) + " "
           + QByteArray::number(i->width(), 'f', 1) + "x" + QByteArray::number(i->height(), 'f', 1) + ");";
    n = 0;
    for (QQuickItem *i : std::as_const(w->itemsToPolish))
        if (n++ < 12)
            p += itemName(i) + ";";
    if (!d.isEmpty())
        fprintf(f, "d,%lld,%s\n", (long long)us, d.constData());
    if (!p.isEmpty())
        fprintf(f, "p,%lld,%s\n", (long long)us, p.constData());
}

// Кадр-эхо: всё, что сменилось в сцене, — слои (ShaderEffectSource, он же layer.enabled) с одной пометкой «содержимое».
// Так Qt отзывается на собственное обновление слоя: слой перерисован в кадре — его элемент просит ещё кадр, а в нём
// уже нечего менять. Страница браузера идёт через слой (скруглённые углы окна), и каждый её кадр стол рисовал,
// паковал и сжимал дважды: ролик 25 к/с — 60 кадров трансляции, больше половины — точные повторы (docs/DESKTOP.md,
// «Стол на полную», «Кадры-эхо»). NAOMI_DESK_ECHO=1 — рисовать их, как раньше (для сравнения)
bool echoFrame(QQuickWindow *window)
{
    static const bool keep = qEnvironmentVariableIntValue("NAOMI_DESK_ECHO") != 0;
    QQuickWindowPrivate *w = QQuickWindowPrivate::get(window);
    if (keep || !w->dirtyItemList || !w->itemsToPolish.isEmpty())
        return false;
    for (QQuickItem *i = w->dirtyItemList; i; i = QQuickItemPrivate::get(i)->nextDirtyItem)
        if (QQuickItemPrivate::get(i)->dirtyAttributes != QQuickItemPrivate::Content
            || !i->inherits("QQuickShaderEffectSource"))
            return false;
    return true;
}

// Фокус клавиатуры: окно за кадром фокуса системы не получает никогда, а Qt даёт фокус элементам, только если
// окно (или то, в котором его показывают, — renderWindow) в фокусе. Без этого клавиши зрителя доходили до окна
// стола, но не до его элементов (проверено 13 ноября 2026). Поэтому на время «окно стола в фокусе» (setActive)
// стол для Qt показан в окне телефона — корень сцены берёт фокус и держит его. Всё остальное время renderWindow
// нет: иначе стол рисовался бы с плотностью экрана телефона (а не 1,5) — тоже проверено.
class DeskRenderControl : public QQuickRenderControl
{
public:
    using QQuickRenderControl::QQuickRenderControl;
    bool focusing = false;
    QWindow *renderWindow(QPoint *offset) override
    {
        if (offset)
            *offset = {};
        return focusing ? QGuiApplication::focusWindow() : nullptr;
    }
};

// Часы анимаций стола (docs/DESKTOP.md, «Часы анимаций»). Анимации Qt — одни на поток интерфейса, а в нём и стол, и
// экран телефона; часы Qt шагают кадрами экрана телефона. Телефон у приставки лежит в AOD, панель — в малом
// питании (≈ 30 Гц): меню и окна на мониторе шли 18–20 шагами в секунду (замер 16 ноября 2026). Эти часы шагают,
// когда нужно: перед кадром стола (такт зрителя), после кадра телефона (его анимации) и, если ни того ни другого, —
// по таймеру (паузы, анимации того, чего не видно). Время — настенное, а по тактам — сетка обновлений монитора
// зрителя: такт доходит до потока интерфейса с разбросом ±2 мс, и анимация шагала бы неровно (13,33 ± 2 мс), хотя
// кадры на мониторе встают ровно.
class DeskClock : public QAnimationDriver
{
public:
    explicit DeskClock(QObject *parent)
        : QAnimationDriver(parent)
    {
        m_idle.setSingleShot(true);
        m_idle.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_idle, &QTimer::timeout, this, [this] { step(); });
    }
    void setIdleMs(int ms) { m_idleMs = ms; }
    qint64 lastTickUs() const { return m_lastTickUs; }   // последний такт — на сетке монитора (мкс, steady_clock)
    qint64 elapsed() const override { return isRunning() ? (m_nowUs - m_startUs) / 1000 : 0; }
    void step() { stepTo(steadyUs(), m_idleMs); }
    // такт: value — его обновление монитора (мкс часов зрителя). Время — прошлый такт плюс разность значений
    // (шаг ровно в период монитора), подтянутое к приходу такта на 1/8 расхождения; расхождение больше 8 мс
    // (поток стоял, зритель сменил опережение) — время прихода
    void tick(qint64 arrivedUs, quint64 value)
    {
        qint64 t = arrivedUs;
        if (m_lastValue && value > m_lastValue) {
            const qint64 err = arrivedUs - (m_lastTickUs + qint64(value - m_lastValue));
            if (std::abs(err) < 8000)
                t = arrivedUs - err + err / 8;
        }
        m_lastValue = value;
        m_lastTickUs = t;
        stepTo(t, 3 * m_idleMs);   // таймер — только если такты пропадут: опоздавший такт он бы опередил
    }

protected:
    void start() override
    {
        m_startUs = m_nowUs = steadyUs();
        QAnimationDriver::start();
        m_idle.start(m_idleMs);
    }
    void stop() override
    {
        m_idle.stop();
        QAnimationDriver::stop();
    }

private:
    void stepTo(qint64 us, int idleMs)
    {
        if (!isRunning())
            return;
        m_nowUs = std::max(m_nowUs, us);   // время анимаций назад не идёт
        advance();
        if (isRunning())
            m_idle.start(idleMs);
    }

    QTimer m_idle;
    int m_idleMs = 13;
    qint64 m_startUs = 0, m_nowUs = 0;     // мкс, steady_clock
    quint64 m_lastValue = 0;
    qint64 m_lastTickUs = 0;
};

// Сборка QML по частям (incubateObject, Loader asynchronous — запас «Файлов», разделы «Настроек»), пока стол смотрят.
// Свой контроллер Qt шагает кусками по 2/3 кадра экрана телефона (≈ 7 мс при 90 Гц, на деле до 11) по таймеру, не
// зная тактов стола: такт ждал в очереди, кадр стола опаздывал (прогон стола 19 ноября 2026: куски по 6–11 мс, 72 за
// прогон). Этот — сразу после кадра стола, пока до следующего такта есть время (не больше kSliceUs), а без кадров —
// по таймеру такими же кусками (стол не смотрят — только так)
class DeskIncubator : public QObject, public QQmlIncubationController
{
public:
    static constexpr qint64 kSliceUs = 3000;
    explicit DeskIncubator(QObject *parent)
        : QObject(parent)
    {
        m_idle.setSingleShot(true);
        m_idle.setInterval(6);
        QObject::connect(&m_idle, &QTimer::timeout, this, [this] { slice(kSliceUs); });
    }
    // после кадра стола: до следующего такта (или кадра по сетке) — untilUs; часть — сборке, с запасом 4 мс
    void afterFrame(qint64 untilUs)
    {
        if (incubatingObjectCount())
            slice(std::clamp<qint64>(untilUs - 4000, 0, kSliceUs));
    }

protected:
    void incubatingObjectCountChanged(int count) override
    {
        if (count && !m_idle.isActive())
            m_idle.start();
    }

private:
    void slice(qint64 us)
    {
        if (us >= 500)
            incubateFor(int(us / 1000));
        if (incubatingObjectCount())
            m_idle.start();   // кадров нет — дальше по таймеру; пойдут кадры — их паузы раньше
    }
    QTimer m_idle;
};

} // namespace

namespace {
// четверть суперэллипса |x|^5 + |y|^5 = 1 (bcdesk, corner_path): 17 точек, (cos t)^0,4 и (sin t)^0,4
const std::array<QPointF, 17> &unitCorner()
{
    static const std::array<QPointF, 17> u = [] {
        std::array<QPointF, 17> a;
        for (int j = 0; j <= 16; ++j) {
            const double t = j / 16.0 * M_PI / 2;
            a[size_t(j)] = QPointF(std::pow(std::max(std::cos(t), 0.0), 0.4), std::pow(std::max(std::sin(t), 0.0), 0.4));
        }
        return a;
    }();
    return u;
}
} // namespace

QPolygonF DesktopScreen::outline(qreal x, qreal y, qreal w, qreal h, qreal e) const
{
    const auto &u = unitCorner();
    struct C { qreal cx, cy; int sx, sy; bool down; };
    const C corners[] = {{x + w - e, y + e, 1, -1, true}, {x + w - e, y + h - e, 1, 1, false},
                         {x + e, y + h - e, -1, 1, true}, {x + e, y + e, -1, -1, false}};
    QPolygonF pts;
    pts.reserve(70);
    pts << QPointF(x + e, y);
    for (const C &c : corners)
        for (int j = 0; j <= 16; ++j) {
            const QPointF &p = u[size_t(c.down ? 16 - j : j)];
            pts << QPointF(c.cx + c.sx * e * p.x(), c.cy + c.sy * e * p.y());
        }
    pts << pts.first();
    return pts;
}

QPolygonF DesktopScreen::outlineTop(qreal x, qreal y, qreal w, qreal h, qreal e) const
{
    const auto &u = unitCorner();
    QPolygonF pts;
    pts.reserve(38);
    auto arc = [&](qreal cx, int sx, bool down) {
        for (int j = 0; j <= 16; ++j) {
            const QPointF &p = u[size_t(down ? 16 - j : j)];
            pts << QPointF(cx + sx * e * p.x(), y + e - e * p.y());
        }
    };
    arc(x + w - e, 1, true);
    pts << QPointF(x + w, y + h) << QPointF(x, y + h);
    arc(x + e, -1, false);
    pts << pts.first();
    return pts;
}

FILE *DesktopScreen::trace()
{
    return g_trace;
}

bool DesktopScreen::setTrace(const QString &path, bool scene)
{
    if (g_trace)
        fclose(g_trace);
    g_trace = path.isEmpty() ? nullptr : fopen(path.toLocal8Bit().constData(), "w");
    g_traceScene = scene;
    return path.isEmpty() || g_trace;
}

DesktopScreen::DesktopScreen(QQmlEngine *engine, const QString &qml, QSize logical, qreal dpr,
                             QQuickWindow *nativeWindow, int sceneRotation, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_qml(qml)
    , m_logical(SceneRotation::isLandscape(sceneRotation) ? logical.transposed() : logical)
    , m_pixels(frameSize(m_logical, dpr))
    , m_dpr(dpr)
    , m_native(nativeWindow != nullptr)
    , m_sceneRotation(sceneRotation)
{
    if (m_native) {
        m_window = nativeWindow;
        m_window->resize(logical);
        return;
    }
    m_frame.setSingleShot(true);
    m_frame.setTimerType(Qt::PreciseTimer);
    // кадр стола — не чаще 90 в секунду (NAOMI_DESK_FPS — иначе: разведка «реже, но чище»; зритель — Want)
    bool ok = false;
    const int fps = qEnvironmentVariableIntValue("NAOMI_DESK_FPS", &ok);
    m_periodUs = 1000000 / (ok && fps > 0 ? fps : 90);
    connect(&m_frame, &QTimer::timeout, this, &DesktopScreen::render);
    m_defaultFps.setSingleShot(true);
    m_defaultFps.setInterval(2000);
    connect(&m_defaultFps, &QTimer::timeout, this, [this] {
        m_fpsKnown = true;
        setOutputMode();
    });
    auto *clock = new DeskClock(this);
    clock->setIdleMs(int(m_periodUs / 1000) + 2);
    m_clock = clock;
    m_control = new DeskRenderControl(this);
    m_window = new QQuickWindow(m_control);
    m_window->setGeometry(0, 0, m_logical.width(), m_logical.height());
    m_window->setColor(Qt::black);
}

DesktopScreen::~DesktopScreen()
{
    if (m_ownClock)
        setClock(false);
    if (m_incubator && m_engine->incubationController() == m_incubator)
        m_engine->setIncubationController(nullptr);   // движок переживёт стол
    delete m_root;
    if (!m_native)
        delete m_window;
    delete m_control;
    // текстура и контекст — до выхода процесса (контекст уходит вместе с ним)
}

// Вывод в пикселях кадра стола (1920 × 1080), масштаб 1: программа стола рисует в пикселях (QT_SCALE_FACTOR 1,5),
// стол показывает её буфер 1:1 — как окна программ на телефоне (docs/SHELL.md, «Окна программ»). «Рисуй дальше»
// (frame callback) стол шлёт сам — сразу после sync, а не после отрисовки, как Qt (render): программа рисует свой
// кадр, пока стол рисует свой. Место — ниже экрана телефона: выводы не перекрываются.
void DesktopScreen::setCompositor(QWaylandCompositor *compositor, QWaylandQuickOutput *nativeOutput)
{
    m_compositor = compositor;
    m_output = m_native ? nativeOutput : new QWaylandQuickOutput(compositor, m_window);
    Q_ASSERT(m_output);
    m_output->setAutomaticFrameCallback(m_native);
    m_output->setSizeFollowsWindow(false);
    setOutputMode();
    m_output->setPosition(m_native ? QPoint() : QPoint(0, 10000));
    m_output->setManufacturer(QStringLiteral("Naomi"));
    m_output->setModel(QStringLiteral("desktop"));
    // Вывод для программ Windows (docs/WINDOWS.md, «Выводы»): «экран» 1280 × 720, виден только им (фильтр —
    // AndroidApps::filterGlobal). Wine предлагает игре ровно те режимы, что объявил вывод, а кадр режима меньше
    // текущего сам растягивает до текущего и отдаёт уже большим: с выводом стола 2400 × 1350 игра 1280 × 720 гнала
    // через память 13 МБ на кадр. С этим выводом 1280 × 720 — родной режим, кадр остаётся малым, до окна его
    // дотягивает стол. Окна на нём не показываются: кадры и их такт — у вывода стола
    m_winOutput = new QWaylandOutput(compositor, nullptr);
    const QWaylandOutputMode winMode(QSize(1280, 720), 60000);
    m_winOutput->addMode(winMode, true);
    m_winOutput->setCurrentMode(winMode);
    m_winOutput->setPosition(QPoint(0, 20000));
    m_winOutput->setManufacturer(QStringLiteral("Naomi"));
    m_winOutput->setModel(QStringLiteral("windows"));
    // Отметки показа (wp_presentation) — кадрам стола (render). Waydroid ставит по ним фазу своей развёртки (vsync
    // Android), а период берёт из частоты вывода — раз, при включении Android. Без них Android шёл по 90 Гц экрана
    // телефона: на мониторе 75 Гц кадры игры ложились 11, 11, 11, 11, 22 мс (замер 18 ноября 2026)
    m_presentation = new QWaylandPresentationTime(compositor);
    m_presentation->initialize();
}

bool DesktopScreen::showNative()
{
    if (!m_native || !loadScene())
        return false;
    resizeNative();
    connect(m_window, &QWindow::widthChanged, this, &DesktopScreen::resizeNative);
    connect(m_window, &QWindow::heightChanged, this, &DesktopScreen::resizeNative);
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        const qint64 now = steadyUs();
        m_prevUs = now;
        if (m_presentation)
            m_presentation->sendFeedback(m_window, ++m_presented, quint64(now / 1000000), quint32(now % 1000000 * 1000));
    }, Qt::QueuedConnection);
    m_ready = m_active = true;
    m_window->showFullScreen();
    return true;
}

void DesktopScreen::setSceneRotation(int rotation)
{
    if (!m_native || rotation == m_sceneRotation)
        return;
    m_sceneRotation = rotation;
    resizeNative();
}

void DesktopScreen::resizeNative()
{
    const QSize size = SceneRotation::isLandscape(m_sceneRotation) ? m_window->size().transposed() : m_window->size();
    if (size == m_logical)
        return;
    m_logical = size;
    m_pixels = frameSize(size, m_dpr);
    m_root->setSize(size);
    setOutputMode();
    emit pixelsChanged();
}

QPointF DesktopScreen::mapToWindow(QPointF point) const
{
    return m_native && m_root ? m_root->mapToScene(point) : point;
}

void DesktopScreen::setNativeRefreshRate(double hz)
{
    if (!m_native || hz < 1 || (m_fpsKnown && qRound(hz) == rateHz()))
        return;
    m_periodUs = 1000000 / qRound(hz);
    m_fpsKnown = true;
    setOutputMode();
}

QWaylandOutput *DesktopScreen::output() const
{
    return m_output;
}

// Режим вывода: кадр стола в пикселях и частота — кадров стола в секунду (мГц), какую назвал зритель (Want; не назвал
// за 2 с — по умолчанию сеанса, setMaxFps). До того — 1 Гц: Qt шлёт программе все режимы вывода, что были (убрать
// нельзя), а Waydroid берёт наибольшую частоту из них — 90 Гц по умолчанию перебивали бы 75 монитора. 1 Гц Waydroid не
// берёт (ему нужно больше 1 Гц). Зрители с разной частотой за одну жизнь оболочки — у Android наибольшая из них
void DesktopScreen::setOutputMode()
{
    if (!m_output)
        return;
    const int millihertz = m_native ? rateHz() * 1000 : m_fpsKnown ? int(1000000000LL / m_periodUs) : 1000;
    const QWaylandOutputMode mode(m_pixels, millihertz);
    if (!m_output->modes().contains(mode))
        m_output->addMode(mode, m_output->modes().isEmpty());
    m_output->setCurrentMode(mode);
}

QSize DesktopScreen::frameSize(QSize logical, double scale)
{
    return QSize(int(std::lround(logical.width() * scale / 2)) * 2, int(std::lround(logical.height() * scale / 2)) * 2);
}

void DesktopScreen::setGeometry(QSize logical, double scale)
{
    if (m_native || (logical == m_logical && scale == m_dpr))
        return;
    const bool rescaled = scale != m_dpr;
    m_logical = logical;
    m_dpr = scale;
    m_pixels = frameSize(logical, scale);
    if (rescaled)
        emit scaleChanged();
    emit pixelsChanged();
    m_window->setGeometry(0, 0, m_logical.width(), m_logical.height());
    setOutputMode();
    if (!m_ready)
        return;   // сцены ещё нет — init() возьмёт новый размер
    if (m_context->makeCurrent(m_surface)) {   // новая цель — до того, как старую текстуру отпустить
        const GLuint old = m_texture;
        allocateTarget();
        glDeleteTextures(1, &old);
        m_context->doneCurrent();
    }
    m_window->contentItem()->setSize(m_logical);
    m_root->setSize(m_logical);
    qCInfo(lcDesktop, "стол %dx%d точек, кадр %dx%d (масштаб %.2f)", m_logical.width(), m_logical.height(),
           m_pixels.width(), m_pixels.height(), m_dpr);
    requestFrame();
}

void DesktopScreen::allocateTarget()
{
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, m_pixels.width(), m_pixels.height());
    glBindTexture(GL_TEXTURE_2D, 0);
    QQuickRenderTarget target = QQuickRenderTarget::fromOpenGLTexture(m_texture, m_pixels);
    target.setDevicePixelRatio(m_dpr);
    m_window->setRenderTarget(target);
}

// Контекст стола — с высоким приоритетом на видеочипе (EGL_IMG_context_priority; так свой композитор держит Android):
// задания стола встают в очереди видеочипа впереди заданий программ — игры, браузера — и вытесняют начатые. Иначе под
// полной нагрузкой видеочипа кадр стола ждал за ними: задержанные такты и пропуски монитора (docs/ANDROID-AUDIT.md,
// этап 2). Qt приоритет контексту задать не даёт — создаём контекст EGL сами с той же настройкой (config), что выбрал
// Qt, и отдаём его Qt (QNativeInterface::QEGLContext::fromNative). Не вышло — остаётся контекст Qt.
// NAOMI_GPU_PRIORITY=0 — как раньше
static QOpenGLContext *highPriorityContext(QOpenGLContext *plain, QObject *parent)
{
    if (qEnvironmentVariable("NAOMI_GPU_PRIORITY") == QLatin1String("0"))
        return nullptr;
    auto *native = plain->nativeInterface<QNativeInterface::QEGLContext>();
    if (!native)
        return nullptr;
    const EGLDisplay display = native->display();
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_IMG_context_priority")) {
        qCInfo(lcDesktop, "видеочип: приоритета контекста нет (EGL_IMG_context_priority)");
        return nullptr;
    }
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                                 EGL_CONTEXT_PRIORITY_HIGH_IMG, EGL_NONE};
    const EGLContext context = eglCreateContext(display, native->config(), EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT) {
        qCWarning(lcDesktop, "видеочип: контекст с приоритетом не создан (0x%x)", eglGetError());
        return nullptr;
    }
    QOpenGLContext *high = QNativeInterface::QEGLContext::fromNative(context, display);
    if (!high || !high->isValid()) {
        delete high;
        eglDestroyContext(display, context);
        qCWarning(lcDesktop, "видеочип: Qt не принял контекст с приоритетом");
        return nullptr;
    }
    high->setParent(parent);
    EGLint level = 0;
    eglQueryContext(display, context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &level);
    qCInfo(lcDesktop, "видеочип: контекст стола — приоритет %s", level == EGL_CONTEXT_PRIORITY_HIGH_IMG ? "высокий"
           : level == EGL_CONTEXT_PRIORITY_MEDIUM_IMG ? "обычный (высокий не дали)" : "низкий");
    return high;
}

// Свой контекст GL (ES 3.0, как у окна телефона — eglfs_naomi) и буфер без окна: стол не на экране
bool DesktopScreen::init()
{
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGLES);
    f.setVersion(3, 0);
    f.setRedBufferSize(8);
    f.setGreenBufferSize(8);
    f.setBlueBufferSize(8);
    f.setAlphaBufferSize(8);
    m_context = new QOpenGLContext(this);
    m_context->setFormat(f);
    m_surface = new QOffscreenSurface(nullptr, this);
    m_surface->setFormat(f);
    m_surface->create();
    if (m_context->create())
        if (QOpenGLContext *high = highPriorityContext(m_context, this)) {
            delete m_context;
            m_context = high;
        }
    if (!m_context->isValid() || !m_context->makeCurrent(m_surface)) {
        qCWarning(lcDesktop, "контекст GL для стола не создан");
        return false;
    }
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_context));
    if (!m_control->initialize()) {
        qCWarning(lcDesktop, "Qt Quick для стола не запустился");
        return false;
    }
    allocateTarget();

    const bool loaded = loadScene();
    if (loaded) {
        m_window->contentItem()->setSize(m_logical);
        // сцена что-то поменяла (анимация, часы, наведение) — кадр
        connect(m_control, &QQuickRenderControl::renderRequested, this, &DesktopScreen::requestFrame);
        connect(m_control, &QQuickRenderControl::sceneChanged, this, &DesktopScreen::requestFrame);
    }
    m_context->doneCurrent();
    return loaded;
}

bool DesktopScreen::loadScene()
{
    QQmlComponent component(m_engine, QUrl::fromLocalFile(m_qml));
    m_root = qobject_cast<QQuickItem *>(component.create(m_engine->rootContext()));
    if (!m_root) {
        qCWarning(lcDesktop) << "стол:" << component.errors();
        return false;
    }
    m_root->setParentItem(m_window->contentItem());
    m_root->setSize(m_logical);
    qCInfo(lcDesktop, "стол %dx%d точек, кадр %dx%d", m_logical.width(), m_logical.height(), m_pixels.width(),
           m_pixels.height());
    return true;
}

void DesktopScreen::setActive(bool on)
{
    if (m_native || on == m_active)
        return;
    m_active = on;
    if (!on) {
        m_frame.stop();
        setClock(false);
        if (auto captured = std::exchange(m_capture, nullptr))   // скриншот кадра не дождётся
            captured(QImage());
        return;
    }
    if (!m_ready && !m_failed) {
        m_ready = init();
        m_failed = !m_ready;
    }
    if (!m_ready)
        return;
    setClock(true);
    if (!m_incubator) {   // с первого показа стола — насовсем (и для сборок экрана телефона: куски те же по таймеру)
        m_incubator = new DeskIncubator(this);
        m_engine->setIncubationController(m_incubator);
    }
    // окно стола «в фокусе»: клавиши зрителя — его полям
    QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    static_cast<DeskRenderControl *>(m_control)->focusing = true;
    QCoreApplication::sendEvent(m_window, &focus);
    static_cast<DeskRenderControl *>(m_control)->focusing = false;
    requestFrame();
}

bool DesktopScreen::makeCurrent()
{
    return m_context && m_context->makeCurrent(m_surface);
}

void DesktopScreen::doneCurrent()
{
    if (m_context)
        m_context->doneCurrent();
}

QImage DesktopScreen::grab()
{
    if (m_native) {
        const QImage image = m_window->grabWindow();
        return m_sceneRotation ? image.transformed(QTransform().rotate(-m_sceneRotation)) : image;
    }
    if (!m_ready || !m_texture || !makeCurrent())
        return {};
    QImage image = readTexture(QRect(QPoint(), m_pixels));
    doneCurrent();
    return image.flipped(Qt::Vertical);   // у цели Qt Quick первая строка — низ
}

QImage DesktopScreen::readTexture(const QRect &pixels)
{
    const QRect r = pixels.intersected(QRect(QPoint(), m_pixels));
    if (r.isEmpty())
        return {};
    GLint was = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    QImage image(r.size(), QImage::Format_RGBA8888_Premultiplied);   // строка — ширина × 4: выравнивание GL (4) то же
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        glReadPixels(r.x(), m_pixels.height() - r.y() - r.height(), r.width(), r.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                     image.bits());
    else
        image = QImage();
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(was));
    glDeleteFramebuffers(1, &fbo);
    return image;
}

bool DesktopScreen::captureNext(const QRect &pixels, std::function<void(QImage)> done)
{
    if (!m_active || !m_ready)
        return false;
    if (m_native) {
        QTimer::singleShot(0, this, [this, pixels, done = std::move(done)] {
            const QImage image = grab();
            done(image.isNull() ? QImage() : image.copy(pixels).flipped(Qt::Vertical));
        });
        return true;
    }
    m_captureRect = pixels;
    m_capture = std::move(done);
    requestFrame();   // сцена могла ничего не просить — кадр всё равно будет
    return true;
}

// Часы анимаций потока интерфейса (DeskClock). Свои — пока стол смотрят: часы Qt (у его цикла отрисовки) снимаются,
// свои ставятся; Qt переносит ход времени сам — анимации не прыгают. Шаг и после кадра телефона: его анимации идут
// своим ходом, в миг его кадра (afterSynchronizing — в потоке отрисовки, шаг — следом, в потоке интерфейса, как у Qt).
void DesktopScreen::setClock(bool own)
{
    QAnimationDriver *qt = QSGRenderLoop::instance()->animationDriver();
    if (own == m_ownClock || !qt)
        return;
    m_ownClock = own;
    if (own) {
        qt->uninstall();
        m_clock->install();
        if (m_phone)
            m_phoneFrames = connect(m_phone, &QQuickWindow::afterSynchronizing, m_clock, [this] { stepClock(); },
                                    Qt::QueuedConnection);
    } else {
        disconnect(m_phoneFrames);
        m_clock->uninstall();
        qt->install();
    }
    qCInfo(lcDesktop, "часы анимаций — %s", own ? "свои (шаг в миг кадра стола)" : "Qt (кадрами экрана телефона)");
}

int DesktopScreen::idleMs() const
{
    return int(std::min<qint64>((steadyUs() - m_prevUs) / 1000, 1000000));
}

void DesktopScreen::stepClock()
{
    if (m_ownClock)
        static_cast<DeskClock *>(m_clock)->step();
}

void DesktopScreen::setMaxFps(int fps, bool named)
{
    m_periodUs = 1000000 / qBound(30, fps, 120);
    static_cast<DeskClock *>(m_clock)->setIdleMs(int(m_periodUs / 1000) + 2);
    // Частоту по умолчанию сеанса (90) — выводу, только если зритель за 2 с свою не назвал (окно Mac Want не шлёт):
    // приставка называет сразу, и 90 перебили бы её 75 у Waydroid
    if (named) {
        m_defaultFps.stop();
        m_fpsKnown = true;
        setOutputMode();
    } else {
        m_defaultFps.start();
    }
    qCInfo(lcDesktop, "кадр стола — не чаще %d к/с", 1000000 / int(m_periodUs));
}

// Такт зрителя (Tick): приставка на Pi задаёт стол в такт своему монитору — кадр сразу, если сцена просила.
// Тактов нет 0,1 с (зритель ушёл, это окно Mac) — стол снова сам по себе, по своей сетке.
constexpr qint64 kTickHoldUs = 100000;

void DesktopScreen::setPulse(bool on)
{
    m_pulseOn = on;
    if (m_native) {
        const auto snapshot = reinterpret_cast<NaomiFrameSnapshotFn>(QGuiApplication::platformFunction(kNaomiFrameSnapshot));
        NaomiFrameSnapshot stats{};
        if (snapshot)
            snapshot(&stats);
        m_pulseFrames = stats.frames;
        m_pulseDrops = stats.drops;
        return;
    }
    m_pulseWork.clear();
    m_pulseMissed = 0;
    m_pulseTickUs = 0;
}

DesktopScreen::Pulse DesktopScreen::takePulse()
{
    if (m_native) {
        const auto snapshot = reinterpret_cast<NaomiFrameSnapshotFn>(QGuiApplication::platformFunction(kNaomiFrameSnapshot));
        NaomiFrameSnapshot stats{};
        if (snapshot)
            snapshot(&stats);
        const int frames = int(stats.frames - std::exchange(m_pulseFrames, stats.frames));
        const int drops = int(stats.drops - std::exchange(m_pulseDrops, stats.drops));
        return {frames, stats.medianMs, stats.maxMs, drops};
    }
    Pulse p;
    p.frames = int(m_pulseWork.size());
    p.missed = std::exchange(m_pulseMissed, 0);
    if (p.frames) {
        std::sort(m_pulseWork.begin(), m_pulseWork.end());
        p.medianMs = m_pulseWork[m_pulseWork.size() / 2] / 1000.0;
        p.worstMs = m_pulseWork.back() / 1000.0;
        m_pulseWork.clear();
    }
    return p;
}

void DesktopScreen::tick(quint64 value)
{
    m_lastTickUs = steadyUs();
    if (m_pulseOn) {   // пропуск — опоздавший такт при сцене, ждущей кадра (desktop_screen.h, Pulse)
        if (const qint64 gap = m_lastTickUs - m_pulseTickUs; m_pulseTickUs && m_dirty && gap > m_periodUs * 3 / 2
                                                             && gap < 500000)
            m_pulseMissed += int((gap + m_periodUs / 2) / m_periodUs) - 1;
        m_pulseTickUs = m_lastTickUs;
    }
    // Что программы стола (браузер) уже прислали — сейчас, до кадра: событие такта и пришедший кадр браузера ждут
    // в очереди потока интерфейса, и такт, разобранный первым, рисовал стол без нового кадра — тот ждал следующего
    // такта (браузер при листании — 40 к/с вместо 75; docs/WEB.md, «Скорость браузера»)
    if (m_compositor && m_active)
        m_compositor->processWaylandEvents();
    if (m_ownClock)   // анимации — на миг такта: сцена с ними просит кадр сейчас же (requestFrame)
        static_cast<DeskClock *>(m_clock)->tick(m_lastTickUs, value);
    if (FILE *f = deskTrace())
        fprintf(f, "t,%lld,%d\n", (long long)m_lastTickUs, int(m_dirty)), fflush(f);
    if (m_dirty && m_active && m_ready) {
        m_frame.stop();
        render();
    }
}

void DesktopScreen::requestFrame()
{
    if (!m_active || !m_ready)
        return;
    if (m_native) {
        m_window->update();
        return;
    }
    if (FILE *f = deskTrace(); f && !m_dirty)   // первая просьба после кадра (их сотни за кадр анимации)
        fprintf(f, "q,%lld,0\n", (long long)steadyUs()), fflush(f);
    m_dirty = true;
    if (steadyUs() - m_lastTickUs < kTickHoldUs) {   // по такту зрителя; таймер — подстраховка, если такты пропадут
        if (!m_frame.isActive())
            m_frame.start(int(kTickHoldUs / 1000));
        return;
    }
    if (m_frame.isActive())
        return;
    const qint64 wait = m_nextUs - steadyUs();
    m_frame.start(int(qBound<qint64>(qint64(0), (wait + 500) / 1000, m_periodUs / 1000 + 1)));
}

// Кадр: всё в потоке интерфейса, своим контекстом. Служба трансляции снимает кадр в afterRendering окна.
void DesktopScreen::render()
{
    if (!m_active || !m_context->makeCurrent(m_surface))
        return;
    // анимации — на миг кадра, если он не по такту (по такту — уже, по сетке тактов); их просьба о кадре — этот же
    if (steadyUs() - m_lastTickUs >= kTickHoldUs)
        stepClock();
    m_frame.stop();
    m_dirty = false;
    // Сетка кадров: следующий — через период по сетке, а не от этого кадра, — частота точная (75 к/с — 13,33 мс),
    // хоть таймер и в миллисекундах. Кадр сильно позже сетки (сцена стояла) — сетка заново от него.
    const qint64 now = steadyUs();
    m_nextUs = now - m_nextUs > m_periodUs / 2 ? now + m_periodUs : m_nextUs + m_periodUs;
    if (FILE *f = deskTrace(); f && g_traceScene)
        traceScene(f, m_window, now);
    // эхо прошлого кадра — не рисуем (и не снимаем для трансляции): только sync, чтобы сцена приняла пометки.
    // Два подряд не пропускаем: за пропущенным кадром — всегда настоящий
    const bool echo = !m_capture && !m_echoSkipped && echoFrame(m_window);
    m_echoSkipped = echo;
    m_control->polishItems();
    const qint64 b = steadyUs();
    m_control->beginFrame();
    m_control->sync();
    // «рисуй дальше» программам стола — сразу после sync (их буферы уже взяты в сцену), а не после отрисовки: так
    // браузер получал его на ~5 мс позже и, рисуя свой кадр 7–8 мс, не успевал к следующему такту
    if (m_output)
        m_output->sendFrameCallbacks();
    const qint64 c = steadyUs();
    if (!echo)
        m_control->render();
    const qint64 d = steadyUs();
    m_control->endFrame();
    // скриншот: этот кадр, как его получит зритель (afterRendering — уже упакован для кодера)
    QImage shot;
    std::function<void(QImage)> captured;
    if (m_capture) {
        captured = std::move(m_capture);
        m_capture = nullptr;
        shot = readTexture(m_captureRect);
    }
    m_context->doneCurrent();
    if (captured)
        captured(std::move(shot));
    // Отметка показа: этот кадр стола взял буферы программ (sync) — в миг такта (по сетке монитора; без тактов — в миг
    // кадра). Кадр-эхо — тоже: буферы взяты. Развёртка Android — в эти мгновения, и его кадр (SurfaceFlinger отдаёт
    // слои после неё) приходит на 5–9 мс позже такта — посередине между тактами (такт — 13,3 мс; замер 18 ноября 2026)
    if (m_presentation) {
        const qint64 at = steadyUs() - m_lastTickUs < kTickHoldUs ? static_cast<DeskClock *>(m_clock)->lastTickUs() : now;
        m_presentation->sendFeedback(m_window, ++m_presented, quint64(at / 1000000), quint32(at % 1000000 * 1000));
    }
    const qint64 end = steadyUs();
    if (m_incubator) {   // сборка QML — в паузе до следующего такта (или кадра по сетке без тактов)
        const qint64 next = end - m_lastTickUs < kTickHoldUs ? m_lastTickUs + m_periodUs : m_nextUs;
        static_cast<DeskIncubator *>(m_incubator)->afterFrame(next - steadyUs());
    }
    if (echo) {
        ++m_statEcho;
        if (FILE *f = deskTrace())
            fprintf(f, "e,%lld\n", (long long)now), fflush(f);
        return;
    }
    if (FILE *f = deskTrace())
        fprintf(f, "r,%lld,%lld,%lld,%lld,%lld\n", (long long)now, (long long)(b - now), (long long)(c - b),
                (long long)(d - c), (long long)(end - d)), fflush(f);
    // Раз в секунду, пока стол рисует, — строка в журнал: кадров, кадр в потоке интерфейса (медиана, худший), шаги
    // в движении (между кадрами меньше 0,1 с; больше — сцена стояла) длиннее 20 мс и самый длинный из них. Рывки
    // анимаций при долгой работе с браузером (docs/PI.md, «Анимации и след прозрачного») после перезапуска оболочки
    // не повторились — разбирать по журналу, когда случатся
    if (const qint64 gap = now - m_prevUs; gap < 100000) {
        m_statGapUs = std::max(m_statGapUs, gap);
        m_statLate += gap > 20000;
    }
    m_prevUs = now;
    m_statWork.push_back(int(end - now));
    if (m_pulseOn)
        m_pulseWork.push_back(int(end - now));
    if (now - m_statFrom >= 1000000) {
        std::sort(m_statWork.begin(), m_statWork.end());
        qCInfo(lcDesktop, "стол: кадров %d (эхо не рисовано: %d), в потоке интерфейса %.1f мс (худший %.1f); в движении "
               "шагов > 20 мс: %d, самый длинный %.1f мс", int(m_statWork.size()), m_statEcho,
               m_statWork[m_statWork.size() / 2] / 1000.0, m_statWork.back() / 1000.0, m_statLate, m_statGapUs / 1000.0);
        m_statEcho = 0;
        m_statWork.clear();
        m_statGapUs = 0;
        m_statLate = 0;
        m_statFrom = now;
    }
}
