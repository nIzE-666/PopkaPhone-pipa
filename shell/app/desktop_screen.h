// НаомиФон — рабочий стол: на pipa рисуется в основном окне, на redfin — во втором выводе для трансляции.
// Его нет на панели телефона: сцена Qt Quick (qml/desktop/Desktop.qml, тот же движок QML, что у оболочки)
// рисуется в текстуру своим контекстом OpenGL ES (QQuickRenderControl) в потоке интерфейса — и только пока
// его смотрят (setActive); телефон в это время живёт своей жизнью. Кадр берёт служба трансляции (afterRendering
// окна), ввод зрителя — мышь и клавиши — приходит в окно стола. Окна программ стола (браузер) — на своём
// выводе Wayland (setCompositor). Анимации сцены, пока стол смотрят, шагают своими часами — в миг кадра стола, а не
// кадрами экрана телефона (setActive, docs/DESKTOP.md, «Часы анимаций»).
#pragma once

#include "stream_screen.h"

#include <QObject>
#include <QPolygonF>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

class QAnimationDriver;
class QOffscreenSurface;
class QOpenGLContext;
class QQmlEngine;
class QQmlIncubationController;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class QWaylandCompositor;
class QWaylandOutput;
class QWaylandPresentationTime;
class QWaylandQuickOutput;

class DesktopScreen : public QObject, public StreamScreen
{
    Q_OBJECT
    // пикселей кадра в точке: программы стола (браузер) рисуют с таким QT_SCALE_FACTOR — сменился, их перезапускают
    Q_PROPERTY(double scale READ scale NOTIFY scaleChanged)
    // сколько пикселей кадра в точке на самом деле — так Qt кладёт сцену в кадр: ширина кадра / ширина стола, по обеим
    // осям (пиксель в пиксель — «Просмотр», скриншоты). scale от зрителя бывает округлён: приставка шлёт 1,333 при
    // кадре 2400 × 1348 на стол 1800 × 1012 — сцена идёт по 1,33333, и по scale один к одному выходил с размывом
    // на долю пикселя (замер 18 ноября 2026); нижняя доля точки стола (1012 − 1348 / 1,33333) в кадр не попадает
    Q_PROPERTY(double pixelScale READ pixelScale NOTIFY pixelsChanged)
public:
    // logical — размер в точках интерфейса, dpr — пикселей в точке: кадр — logical × dpr
    DesktopScreen(QQmlEngine *engine, const QString &qml, QSize logical, qreal dpr,
                  QQuickWindow *nativeWindow = nullptr, int sceneRotation = 0, QObject *parent = nullptr);
    ~DesktopScreen() override;

    QQuickWindow *window() const override { return m_window; }   // есть сразу; сцена и GL — с первым setActive
    QSize pixelSize() const override { return m_pixels; }
    QSize logicalSize() const override { return m_logical; }
    QPointF mapToWindow(QPointF point) const override;
    // Форма и масштаб по просьбе зрителя (docs/DESKTOP.md, «Форма стола»): окно Mac — высота под окно (1280 точек,
    // масштаб 1,5), приставка на Pi — сколько точек и пикселей под свой монитор. Кадр — новая текстура в своём
    // контексте; сцена, корень QML и вывод Wayland для окон программ — нового размера.
    void setGeometry(QSize logical, double scale) override;
    double scale() const override { return m_dpr; }
    double pixelScale() const { return m_logical.width() > 0 ? double(m_pixels.width()) / m_logical.width() : m_dpr; }
    static QSize frameSize(QSize logical, double scale);   // кадр — чётный (NV12 кодера)
    unsigned texture() const override { return m_texture; }   // кадр (цель отрисовки), GL — в контексте стола

    // окна программ стола (браузер, docs/WEB.md): свой вывод Wayland для окна стола — программа получает «рисуй
    // дальше» после кадра стола, а не телефона. Частота вывода — кадров стола в секунду (зритель просит — Want), и
    // каждый кадр стола — отметка показа (wp_presentation): по ним Android ведёт свою развёртку в такт стола
    // (docs/ANDROID.md, «Кадры в такт стола»)
    void setCompositor(QWaylandCompositor *compositor, QWaylandQuickOutput *nativeOutput = nullptr);
    bool showNative();
    void setSceneRotation(int rotation);
    void setNativeRefreshRate(double hz);
    QWaylandOutput *output() const;
    QWaylandOutput *winOutput() const { return m_winOutput; }   // «экран» программ Windows (desktop_screen.cpp)
    // окно телефона: пока стол смотрят, часы анимаций шагают и после его кадров — у него свои анимации (setActive)
    void setPhoneWindow(QQuickWindow *phone) { m_phone = phone; }

    // смотрят — рисовать (по просьбам сцены, не чаще 90 к/с), анимации — своими часами; нет — стоять, часы — Qt
    void setActive(bool on) override;
    // не чаще fps к/с (зритель просит — Want; по умолчанию 90); названную зрителем — и выводу Wayland стола
    void setMaxFps(int fps, bool named) override;
    void tick(quint64 value) override;  // такт зрителя: кадр — сейчас, если сцена просила; анимации — по value
    void requestFrame() override;
    // свой контекст GL — для того, кто держит в нём ресурсы (служба трансляции освобождает буферы)
    bool makeCurrent() override;
    void doneCurrent() override;
    // последний кадр стола картинкой (снимок для разработки — tools/naomi shot desk): то, что ушло зрителю; стол не
    // смотрят — кадр, на котором он остановился. Пусто — стол ещё ни разу не рисовался
    QImage grab();
    // Скриншот (DeskShot, docs/DESKTOP.md, «Скриншоты»): прямоугольник pixels следующего кадра — тот кадр рисуется
    // обязательно, даже если в сцене ничего не сменилось, и не пропускается как эхо. done — в потоке интерфейса сразу
    // после кадра, с картинкой строками снизу вверх, как у GL (перевернёт поток записи; пустая — не прочли).
    // false — стол сейчас не рисуют (зрителя нет)
    bool captureNext(const QRect &pixels, std::function<void(QImage)> done);
    // сколько мс стол не рисовал: стол в покое — можно делать тяжёлое заранее (запас «Файлов», Desktop.qml)
    Q_INVOKABLE int idleMs() const;
    // контур прямоугольника с «непрерывными» углами (DeskStyle.outline, outlineTop — только верхние) для Shape: при
    // растягивании окна контур строится на каждом шаге для 2–3 фигур, в JS — по 136 Math.pow и 70 Qt.point
    Q_INVOKABLE QPolygonF outline(qreal x, qreal y, qreal w, qreal h, qreal e) const;
    Q_INVOKABLE QPolygonF outlineTop(qreal x, qreal y, qreal w, qreal h, qreal e) const;
    // трасса кадров стола (как NAOMI_DESK_TRACE) — включить в файл или выключить (пустой путь) на ходу; scene — со
    // строками d и p (что сменилось в сцене к кадру). false — файл не открыть
    static bool setTrace(const QString &path, bool scene);
    static FILE *trace();   // открытая трасса или nullptr (долгие события потока интерфейса — ProbedApp)

    // Пульс кадров — для монитора системы (system_monitor.h); считается, только пока просят (setPulse).
    // frames — кадров нарисовано с прошлого takePulse, medianMs и worstMs — кадр в потоке интерфейса, missed —
    // пропуски: такт зрителя пришёл позже полутора периодов (поток интерфейса был занят или такт задержала сеть),
    // а сцена ждала кадра — кадр не попал в свои обновления монитора. Без тактов (окно Mac) пропуски не считаются
    struct Pulse {
        int frames = 0;
        double medianMs = 0, worstMs = 0;
        int missed = 0;
    };
    void setPulse(bool on);
    Pulse takePulse();
    int rateHz() const { return int((1000000 + m_periodUs / 2) / m_periodUs); }   // частота кадров стола (монитора зрителя)

signals:
    void scaleChanged();
    void pixelsChanged();

private:
    bool init();
    bool loadScene();
    void resizeNative();
    void render();
    void setClock(bool own);   // часы анимаций потока интерфейса: свои (стол смотрят) или Qt (кадрами телефона)
    void stepClock();          // анимации — на сейчас (свои часы)

    QQmlEngine *m_engine;
    QString m_qml;
    QSize m_logical, m_pixels;
    qreal m_dpr;
    const bool m_native;
    int m_sceneRotation;
    QQuickRenderControl *m_control = nullptr;
    qint64 m_periodUs = 1000000 / 90;   // кадр стола не чаще (90 к/с)
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_root = nullptr;
    QWaylandCompositor *m_compositor = nullptr;
    QWaylandQuickOutput *m_output = nullptr;
    QWaylandOutput *m_winOutput = nullptr;
    void setOutputMode();      // вывод стола: размер кадра и частота кадров стола
    bool m_fpsKnown = false;   // частота кадров стола — выводу (зритель назвал или не назвал за 2 с)
    QTimer m_defaultFps;       // зритель частоту пока не назвал — через 2 с выводу частота по умолчанию
    QWaylandPresentationTime *m_presentation = nullptr;
    quint64 m_presented = 0;   // отметок показа отправлено (номер кадра для wp_presentation)
    QQuickWindow *m_phone = nullptr;
    QAnimationDriver *m_clock = nullptr;   // свои часы анимаций (DeskClock)
    // сборка QML по частям — в паузах между кадрами стола и по таймеру мелкими кусками (DeskIncubator)
    QQmlIncubationController *m_incubator = nullptr;
    bool m_ownClock = false;
    QMetaObject::Connection m_phoneFrames;
    void allocateTarget();   // текстура кадра и цель отрисовки под m_pixels (контекст — текущий)
    QImage readTexture(const QRect &pixels);   // часть кадра картинкой, строки снизу вверх (контекст — текущий)
    QRect m_captureRect;                       // captureNext: что снять со следующего кадра
    std::function<void(QImage)> m_capture;
    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    unsigned m_texture = 0;
    bool m_ready = false, m_failed = false, m_active = false;
    QTimer m_frame;            // следующий кадр — не раньше 11 мс после прошлого (90 к/с: кодер тогда быстрее)
    qint64 m_nextUs = 0;   // сетка кадров: когда можно следующий (мкс, steady_clock)
    bool m_dirty = false;      // сцена просила кадр, он ещё не нарисован
    qint64 m_lastTickUs = 0;   // последний такт зрителя: пока они идут, кадр — по такту, а не по своему таймеру
    // строка в журнал раз в секунду: кадр в потоке интерфейса (мкс), шаги в движении длиннее 20 мс, самый длинный
    std::vector<int> m_statWork;
    int m_statLate = 0;
    int m_statEcho = 0;          // за секунду: кадров-эхо не нарисовано (render)
    bool m_echoSkipped = false;  // прошлый кадр был эхом и пропущен
    qint64 m_statGapUs = 0, m_statFrom = 0, m_prevUs = 0;
    bool m_pulseOn = false;
    quint64 m_pulseFrames = 0, m_pulseDrops = 0;
    std::vector<int> m_pulseWork;
    int m_pulseMissed = 0;
    qint64 m_pulseTickUs = 0;
};
