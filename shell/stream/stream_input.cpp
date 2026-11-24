#include "stream_input.h"

#include "stream_protocol.h"
#include "stream_screen.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QtGui/private/qeventpoint_p.h>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QLineF>
#include <QWaylandClient>
#include <QWaylandCompositor>
#include <QWaylandKeyboard>
#include <QWaylandQuickItem>
#include <QWaylandSeat>
#include <QtWaylandCompositor/private/qwaylandkeyboard_p.h>
#include <wayland-server-protocol.h>
#include <QWaylandSurface>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstring>
#include <tuple>

namespace {

ulong eventTime()
{
    static QElapsedTimer t;
    if (!t.isValid())
        t.start();
    return ulong(t.elapsed());
}

} // namespace

StreamInput::StreamInput(QObject *parent)
    : QObject(parent)
{
    m_touch = new QPointingDevice(QStringLiteral("naomi-stream touch"), 0x6e730001,
                                  QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
                                  QInputDevice::Capability::Position | QInputDevice::Capability::Area
                                      | QInputDevice::Capability::NormalizedPosition,
                                  10, 0, QString(), QPointingDeviceUniqueId(), this);
    QWindowSystemInterface::registerInputDevice(m_touch);
    // трекпад зрителя: только прокрутка (точный сдвиг) и наведение — чтобы прокрутка дошла до окна программы
    m_mouse = new QPointingDevice(QStringLiteral("naomi-stream trackpad"), 0x6e730002,
                                  QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Generic,
                                  QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                                      | QInputDevice::Capability::PixelScroll,
                                  1, 1, QString(), QPointingDeviceUniqueId(), this);
    QWindowSystemInterface::registerInputDevice(m_mouse);

    m_scrollEnd.setSingleShot(true);
    m_scrollEnd.setInterval(120);
    connect(&m_scrollEnd, &QTimer::timeout, this, [this] { scroll(0, 0, 0, 0, stream::ScrollEnd); });

    m_gestureTimer.setInterval(8);
    connect(&m_gestureTimer, &QTimer::timeout, this, [this] {
        constexpr int kId = 9;
        ++m_gStep;
        const double t = double(m_gStep) / m_gSteps;
        const QPointF p = m_gFrom + (m_gTo - m_gFrom) * t;
        sendTouch(kId, m_gStep >= m_gSteps ? int(QEventPoint::State::Released) : int(QEventPoint::State::Updated), p);
        if (m_gStep >= m_gSteps)
            m_gestureTimer.stop();
    });
}

void StreamInput::setWindow(QQuickWindow *window)
{
    m_window = window;
}

void StreamInput::setDesktop(StreamScreen *desktop)
{
    if (desktop == m_desktop)
        return;
    if (m_desktop && m_buttons)
        pointer(0, m_pointerAt.x() / m_desktop->logicalSize().width(),
                m_pointerAt.y() / m_desktop->logicalSize().height());   // отпустить кнопки на прежнем
    m_desktop = desktop;
    m_buttons = 0;
}

QPointF StreamInput::toDesktop(double x, double y) const
{
    const QSize l = m_desktop->logicalSize();
    return {std::clamp(x, 0.0, 1.0) * l.width(), std::clamp(y, 0.0, 1.0) * l.height()};
}

// окно стола не на экране (QQuickRenderControl): события — ему прямо
void StreamInput::sendToDesktop(QEvent *event)
{
    QCoreApplication::sendEvent(m_desktop->window(), event);
}

// Мышь стола: изменились кнопки — нажатие или отпускание каждой, иначе — движение (и без кнопок: наведение).
// Старшие биты кнопок — Shift, Ctrl (⌘), Alt зрителя: выделение с Ctrl и Shift. Двойной щелчок Qt сам не узнаёт
// (события идут окну прямо, не через систему окон) — второе нажатие той же кнопкой быстро и рядом с первым
// дополняем событием MouseButtonDblClick, как QGuiApplication.
void StreamInput::pointer(int buttons, double x, double y)
{
    if (!m_desktop)
        return;   // экрану телефона мышь — пальцем (Touch)
    m_pointerAt = toDesktop(x, y);
    const QPointF p = m_desktop->mapToWindow(m_pointerAt);
    const Qt::KeyboardModifiers mods((buttons & stream::PointerShift ? Qt::ShiftModifier : Qt::NoModifier)
                                     | (buttons & stream::PointerCtrl ? Qt::ControlModifier : Qt::NoModifier)
                                     | (buttons & stream::PointerAlt ? Qt::AltModifier : Qt::NoModifier));
    buttons &= 7;
    auto qtButtons = [](int b) {
        return Qt::MouseButtons((b & 1 ? Qt::LeftButton : Qt::NoButton) | (b & 2 ? Qt::RightButton : Qt::NoButton)
                                | (b & 4 ? Qt::MiddleButton : Qt::NoButton));
    };
    const int changed = buttons ^ m_buttons;
    if (!changed) {
        QMouseEvent e(QEvent::MouseMove, p, p, p, Qt::NoButton, qtButtons(buttons), mods);
        sendMouse(e);
        emit pointerSent();
        return;
    }
    int state = m_buttons;
    for (int bit : {1, 2, 4}) {
        if (!(changed & bit))
            continue;
        state ^= bit;
        const Qt::MouseButton button = bit == 1 ? Qt::LeftButton : bit == 2 ? Qt::RightButton : Qt::MiddleButton;
        const bool press = state & bit;
        if (press && !m_buttons && state == bit) {   // первая кнопка: отсюда меряют сдвиг и время удержания
            m_pressAt = p;
            m_pressTime = eventTime();
        }
        QMouseEvent e(press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, p, p, p, button,
                      qtButtons(state), mods);
        sendMouse(e);
        if (press)
            holdModifiers(mods);
        if (!press)
            continue;
        const bool twice = m_click.isValid() && m_clickButton == button
                           && m_click.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval()
                           && QLineF(m_clickAt, p).length() < 6;
        if (twice) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, p, p, p, button, qtButtons(state), mods);
            sendMouse(dbl);
            m_click.invalidate();   // третье нажатие — снова одиночное
        } else {
            m_click.start();
            m_clickAt = p;
            m_clickButton = button;
        }
    }
    m_buttons = buttons;
    if (!buttons)
        releaseModifiers();
    emit pointerSent();
}

// Где нажали и когда — в самом событии. Через систему окон (QWindowSystemInterface) это пишет QGuiApplication, а
// события стола идут окну прямо, и места нажатия у них не было: Qt считал, что кнопку нажали в (0, 0). Любой сдвиг
// мыши с зажатой кнопкой выходил «перетаскиванием» на десятки точек — кнопки на TapHandler (FILE, WEB, N, «+»)
// отменяли щелчок: у настоящей мыши и трекпада курсор при нажатии почти всегда чуть дрожит, и щелчок проходил
// через раз (замечено пользователем 13 ноября 2026; журнал qt.quick.handler.tap: «drag threshold exceeded»).
void StreamInput::sendMouse(QMouseEvent &e)
{
    e.setTimestamp(eventTime());
    QEventPoint &point = e.point(0);
    QMutableEventPoint::setGlobalPressPosition(point, m_pressAt);
    QMutableEventPoint::setPressTimestamp(point, m_pressTime);
    sendToDesktop(&e);
}

// ⌘-щелчок по ссылке в окне программы стола (браузер, docs/WEB.md) — новой вкладкой. Модификаторы мыши программа
// Wayland знает только по своей клавиатуре (wl_keyboard.modifiers), а клавиши стол шлёт ей мимо неё (D-Bus, key).
// Поэтому с нажатия до отпускания всех кнопок она видит зажатые Ctrl и Shift (коды XKB 37, 50), как у bcdesk
// (webframe_cmd_click): ссылку Chromium открывает по отпусканию. Нажатие уже отдано: фокус — у окна программы.
// Шлём только состояние модификаторов (updateModifierState — закрытый метод Qt Wayland): сама клавиша странице не
// нужна, а sendKeyPressEvent состояние не меняет (проверено: wl_keyboard.modifiers не приходил).
void StreamInput::holdModifiers(Qt::KeyboardModifiers mods)
{
    if (!m_held.isEmpty() || !(mods & (Qt::ControlModifier | Qt::ShiftModifier)))
        return;
    auto *app = qobject_cast<QWaylandQuickItem *>(m_desktop->window()->focusObject());
    if (!app || !app->surface() || !app->compositor() || !app->compositor()->defaultSeat()->keyboard())
        return;
    m_heldSeat = app->compositor()->defaultSeat();
    if (mods & Qt::ControlModifier)
        m_held << 37;
    if (mods & Qt::ShiftModifier)
        m_held << 50;
    auto *keyboard = QWaylandKeyboardPrivate::get(m_heldSeat->keyboard());
    for (uint code : std::as_const(m_held))
        keyboard->updateModifierState(code, WL_KEYBOARD_KEY_STATE_PRESSED);
}

void StreamInput::releaseModifiers()
{
    if (m_heldSeat && m_heldSeat->keyboard())
        for (uint code : std::as_const(m_held))
            QWaylandKeyboardPrivate::get(m_heldSeat->keyboard())->updateModifierState(code, WL_KEYBOARD_KEY_STATE_RELEASED);
    m_held.clear();
    m_heldSeat = nullptr;
}

// доли экрана → пиксели экрана (как у сенсора: кадр Qt — 1080 × 2340 или 720 × 1560)
QPointF StreamInput::toNative(double x, double y) const
{
    if (!m_window || !m_window->screen())
        return {};
    const QRect g = m_window->screen()->handle()->geometry();
    return {std::clamp(x, 0.0, 1.0) * (g.width() - 1), std::clamp(y, 0.0, 1.0) * (g.height() - 1)};
}

void StreamInput::sendTouch(int id, int state, QPointF pos)
{
    if (!m_window)
        return;
    const auto st = QEventPoint::State(state);
    if (st == QEventPoint::State::Released)
        m_down.remove(id);
    QList<QWindowSystemInterface::TouchPoint> points;
    const QRect g = m_window->screen()->handle()->geometry();
    auto point = [&](int pid, QEventPoint::State s, QPointF p) {
        QWindowSystemInterface::TouchPoint tp;
        tp.id = pid;
        tp.state = s;
        tp.area = QRectF(0, 0, 8, 8);
        tp.area.moveCenter(p);
        tp.normalPosition = QPointF(p.x() / g.width(), p.y() / g.height());
        tp.pressure = s == QEventPoint::State::Released ? 0 : 1;
        points.append(tp);
    };
    for (auto it = m_down.cbegin(); it != m_down.cend(); ++it)
        if (it.key() != id)
            point(it.key(), QEventPoint::State::Stationary, it.value());
    point(id, st, pos);
    if (st != QEventPoint::State::Released)
        m_down.insert(id, pos);
    QWindowSystemInterface::handleTouchEvent(m_window, eventTime(), m_touch, points);
}

void StreamInput::touch(int phase, int id, double x, double y)
{
    if (m_desktop) {   // стол — мышью: палец — левая кнопка
        pointer(phase == 0 || phase == 1 ? 1 : 0, x, y);
        return;
    }
    const QPointF p = toNative(x, y);
    switch (phase) {
    case 0:
        sendTouch(id, int(m_down.contains(id) ? QEventPoint::State::Updated : QEventPoint::State::Pressed), p);
        break;
    case 1:
        if (m_down.contains(id))
            sendTouch(id, int(QEventPoint::State::Updated), p);
        break;
    default:   // отпустил или отмена
        if (m_down.contains(id))
            sendTouch(id, int(QEventPoint::State::Released), m_down.value(id));
        break;
    }
}

// Прокрутка трекпадом Mac — колесо с точным сдвигом (pixelDelta, в точках интерфейса). Перед ней — наведение
// туда же: окно программы получает прокрутку, только когда курсор Wayland над ним. Конец — от зрителя или сам.
void StreamInput::scroll(double x, double y, double dx, double dy, int phase)
{
    if (m_desktop) {   // стол: колесо с точным сдвигом там, где мышь
        const QSize l = m_desktop->logicalSize();
        const QPointF p = m_desktop->mapToWindow(phase == stream::ScrollEnd ? m_pointerAt : toDesktop(x, y));
        const QPoint pixel(qRound(-dx * l.width()), qRound(-dy * l.height()));
        QWheelEvent e(p, p, pixel, pixel * 8, Qt::NoButton, Qt::NoModifier,
                      phase == stream::ScrollEnd ? Qt::ScrollEnd : Qt::ScrollUpdate, false);
        sendToDesktop(&e);
        return;
    }
    if (!m_window)
        return;
    const QSizeF logical = m_window->size();
    Qt::ScrollPhase qtPhase = Qt::ScrollUpdate;
    if (phase == stream::ScrollEnd) {
        if (!m_scrolling)
            return;
        m_scrolling = false;
        m_scrollEnd.stop();
        qtPhase = Qt::ScrollEnd;
        dx = dy = 0;
    } else {
        if (!m_scrolling) {
            m_scrolling = true;
            m_scrollAt = toNative(x, y);
            QWindowSystemInterface::handleMouseEvent(m_window, eventTime(), m_mouse, m_scrollAt, m_scrollAt,
                                                     Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
            QWindowSystemInterface::handleWheelEvent(m_window, eventTime(), m_mouse, m_scrollAt, m_scrollAt,
                                                     QPoint(), QPoint(), Qt::NoModifier, Qt::ScrollBegin);
        }
        m_scrollEnd.start();
    }
    // dx, dy — сдвиг содержимого как у колеса в браузере (вниз — положительный); у Qt наоборот
    const QPoint pixel(qRound(-dx * logical.width()), qRound(-dy * logical.height()));
    if (qtPhase == Qt::ScrollUpdate && pixel.isNull())
        return;
    QWindowSystemInterface::handleWheelEvent(m_window, eventTime(), m_mouse, m_scrollAt, m_scrollAt, pixel,
                                             pixel * 8, Qt::NoModifier, qtPhase);
}

// Клавиши — прямо объекту с фокусом, как у экранной клавиатуры (keyboard_context). Текст — событием ввода
// (QInputMethodEvent): так печатается любая раскладка. Через окно (QWindowSystemInterface) поле получало букву
// по английской раскладке: «Q» — «q», русские — никак (проверено 13 ноября 2026). Окно программы (клиент Wayland)
// клавиши получает только по английской раскладке (wl_keyboard), а текст ввода — только если слушает протокол
// text-input (наши программы печатают своей клавиатурой и не слушают): программе — своим каналом D-Bus
// (org.naomi.Input.p<pid>, keyboard_context.h), её модуль ввода печатает как своя экранная клавиатура.
void StreamInput::key(bool down, int qtKey, int modifiers, const QString &text)
{
    // клавиши плеера, яркости и блокировки — не тому, что в фокусе: плееру (кто играет), монитору зрителя и
    // выключателю трансляции; одно действие на нажатие, сколько бы ни держали
    if (qtKey == Qt::Key_MediaTogglePlayPause || qtKey == Qt::Key_MediaNext || qtKey == Qt::Key_MediaPrevious
        || qtKey == Qt::Key_ScreenSaver || qtKey == Qt::Key_Sleep || qtKey == Qt::Key_MonBrightnessUp
        || qtKey == Qt::Key_MonBrightnessDown) {
        if (!down) {
            m_onceKeys.removeAll(qtKey);
        } else if (!m_onceKeys.contains(qtKey)) {
            m_onceKeys.append(qtKey);
            if (qtKey == Qt::Key_MonBrightnessUp || qtKey == Qt::Key_MonBrightnessDown) {
                if (m_desktop)
                    emit brightnessKey(qtKey == Qt::Key_MonBrightnessUp ? 1 : -1);
            } else if (qtKey == Qt::Key_ScreenSaver || qtKey == Qt::Key_Sleep)
                emit lockKey(qtKey == Qt::Key_Sleep);
            else
                emit mediaKey(qtKey);
        }
        return;
    }
    // громкость с клавиатуры зрителя — столу (его окно звука и плашка), не тому, что в фокусе; повтор — нажатиями
    if (m_desktop && (qtKey == Qt::Key_VolumeUp || qtKey == Qt::Key_VolumeDown || qtKey == Qt::Key_VolumeMute)) {
        if (down)
            emit volumeKey(qtKey == Qt::Key_VolumeUp ? 1 : qtKey == Qt::Key_VolumeDown ? -1 : 0);
        return;
    }
    // Скриншоты стола, как в НаомиОС: Shift+Alt+3 — весь стол, Shift+Alt+4 — область; у клавиатуры Apple у приставки —
    // и ⌘⇧3 / ⌘⇧4 (⌘ там — Meta). Клавиша — цифрой при любой раскладке: окно Mac шлёт её по месту клавиши, приставка —
    // по латинице на клавише. Сочетание — столу, даже когда в фокусе браузер; его отпускание — тоже (Shift или Alt
    // могли отпустить раньше цифры). Esc при прицеле — отмена прицела, а не клавиша программе
    if (m_desktop && (qtKey == Qt::Key_3 || qtKey == Qt::Key_4)) {
        const auto mods = Qt::KeyboardModifiers::fromInt(modifiers)
                          & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier);
        if (down && (mods == (Qt::ShiftModifier | Qt::AltModifier) || mods == (Qt::ShiftModifier | Qt::MetaModifier))) {
            if (!m_shotKeys.contains(qtKey)) {   // держат — повтор клавиатуры, а снимок один
                m_shotKeys.append(qtKey);
                emit screenshotKey(qtKey == Qt::Key_3 ? 3 : 4);
            }
            return;
        }
        if (!down && m_shotKeys.removeAll(qtKey))
            return;
    }
    if (m_desktop && m_shotSelecting && qtKey == Qt::Key_Escape) {
        if (down)
            emit screenshotKey(0);
        return;
    }
    QObject *focus = m_desktop ? m_desktop->window()->focusObject() : QGuiApplication::focusObject();
    // ⌘C, ⌘V, ⌘X, ⌘Z, ⇧⌘Z, ⌘A клавиатуры Apple (у прочих — Win) — как у Mac: копировать, вставить, вырезать,
    // отменить, повторить, выделить всё. Поля, «Файлы», браузер и Android знают их по Ctrl — им Ctrl-сочетанием (окно
    // Mac шлёт ⌘ Ctrl всегда); тому, кто понимает ⌘ сам (свойство commandKeys: «Терминал» — там Ctrl+C прерывает
    // программу), — как есть. Отпускание и повтор — тем же, чем нажали: ⌘ могли отпустить раньше буквы.
    if (!m_cmdKeys.contains(qtKey) && down
        && (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == Qt::MetaModifier
        && (qtKey == Qt::Key_C || qtKey == Qt::Key_V || qtKey == Qt::Key_X || qtKey == Qt::Key_Z || qtKey == Qt::Key_A)
        && !(focus && focus->property("commandKeys").toBool()))
        m_cmdKeys.insert(qtKey, (modifiers & ~Qt::MetaModifier) | Qt::ControlModifier);
    if (m_cmdKeys.contains(qtKey))
        modifiers = down ? m_cmdKeys.value(qtKey) : m_cmdKeys.take(qtKey);
    if (!focus)
        return;
    if (!m_desktop)
        emit activity();   // стол телефон не будит и гашение не сбрасывает
    if (auto *app = qobject_cast<QWaylandQuickItem *>(focus)) {
        if (!app->surface() || !app->surface()->client())
            return;
        // окно Android (docs/ANDROID.md; свойство androidKeys — у его элемента, фокус же бывает и у элемента слоя
        // внутри): своего канала у него нет — клавишей Wayland (wl_keyboard), как у настоящей клавиатуры: Qt Wayland
        // берёт код клавиши по раскладке композитора (keyToScanCode — английская), символ выбирает раскладка
        // физической клавиатуры в самом Android
        // окно игры (docs/GAMES.md; свойство gameKeys) — тоже клавишами Wayland, но по месту клавиши и без
        // модификаторов: кнопкам геймпада раскладка и Shift не важны
        bool android = false, game = false;
        for (QQuickItem *i = app; i && !android && !game; i = i->parentItem()) {
            android = i->property("androidKeys").toBool();
            game = i->property("gameKeys").toBool();
        }
        if (android) {
            keyToAndroid(app, down, qtKey, modifiers, text);
            return;
        }
        if (game) {
            keyToGame(app, down, qtKey, text);
            return;
        }
        const qint64 pid = app->surface()->client()->processId();
        QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.naomi.Input.p%1").arg(pid),
                                                        QStringLiteral("/input"), QStringLiteral("org.naomi.Input"),
                                                        QStringLiteral("key"));
        m << down << qtKey << modifiers << text;
        QDBusConnection::sessionBus().send(m);   // ответ не нужен
        return;
    }
    // Стол: ⌘V зрителя — Ctrl+V с текстом буфера Mac. Поле ввода его печатает, остальное («Файлы») получает
    // клавишу: вставить файлы. Буква не полю ввода (набор имени в «Файлах», пробел — просмотр) — клавишей с текстом.
    // Клавиши — окну стола: оно отдаёт их тому, что в фокусе, а не принятое — его родителям (так в «Файлах»
    // клавиши окна работают, где бы ни стоял фокус).
    const bool paste = qtKey == Qt::Key_V && (modifiers & Qt::ControlModifier) && !text.isEmpty();
    if ((!qtKey || paste) && !text.isEmpty() && (!m_desktop || acceptsText(focus))) {
        if (down) {
            QInputMethodEvent e;
            e.setCommitString(text);
            QCoreApplication::sendEvent(focus, &e);
        }
        return;
    }
    if (!qtKey && text.size() == 1 && text[0].unicode() < 0x80)
        qtKey = text[0].toUpper().unicode();   // клавиши Qt для латиницы, цифр и пробела — их коды ASCII
    QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, Qt::KeyboardModifiers(modifiers),
                paste ? QString() : text);
    QCoreApplication::sendEvent(m_desktop ? static_cast<QObject *>(m_desktop->window()) : focus, &e);
}

// Символ на английской клавиатуре: код клавиши (evdev) и нужен ли Shift. Нет такого символа — 0
static std::pair<int, bool> usKey(QChar ch)
{
    static const char *const kRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const int kRowStart[] = {16, 30, 44};
    const ushort c = ch.unicode();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        const char lower = char(c | 0x20);
        for (int r = 0; r < 3; ++r)
            if (const char *at = strchr(kRows[r], lower))
                return {kRowStart[r] + int(at - kRows[r]), c < 'a'};
    }
    static const char kDigits[] = "1234567890", kShifted[] = "!@#$%^&*()";
    if (const char *at = c < 0x80 ? strchr(kDigits, char(c)) : nullptr; at && c)
        return {2 + int(at - kDigits), false};
    if (const char *at = c < 0x80 ? strchr(kShifted, char(c)) : nullptr; at && c)
        return {2 + int(at - kShifted), true};
    static const struct { char plain, shifted; int code; } kPunct[] = {
        {'`', '~', 41}, {'-', '_', 12}, {'=', '+', 13}, {'[', '{', 26}, {']', '}', 27}, {'\\', '|', 43},
        {';', ':', 39}, {'\'', '"', 40}, {',', '<', 51}, {'.', '>', 52}, {'/', '?', 53}, {' ', ' ', 57}};
    for (const auto &p : kPunct) {
        if (c == uchar(p.plain))
            return {p.code, false};
        if (c == uchar(p.shifted))
            return {p.code, true};
    }
    return {0, false};
}

// Русская буква на клавиатуре ЙЦУКЕН: код клавиши (evdev) и нужен ли Shift; «№» — Shift+3. Нет — 0
static std::pair<int, bool> ruKey(QChar ch)
{
    static const char16_t kLetters[] = u"йцукенгшщзхъфывапролджэячсмитьбюё";
    static const int kCodes[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34, 35, 36, 37, 38,
                                 39, 40, 44, 45, 46, 47, 48, 49, 50, 51, 52, 41};
    if (ch.unicode() == 0x2116)
        return {4, true};
    const QChar lower = ch.toLower();
    for (int i = 0; kLetters[i]; ++i)
        if (lower.unicode() == kLetters[i])
            return {kCodes[i], ch != lower};
    return {0, false};
}

// Клавиша окну игры (naomi-game, docs/GAMES.md): кнопка геймпада — место клавиши на клавиатуре (код evdev), какая бы
// ни была раскладка: «X» и русская «Ч» — одна клавиша. Зритель шлёт буквы текстом — код берём по букве (английской
// или русской); стрелки, Enter, пробел — по клавише Qt. Shift, Caps и прочие модификаторы игре не нужны.
void StreamInput::keyToGame(QWaylandQuickItem *app, bool down, int qtKey, const QString &text)
{
    QWaylandSeat *seat = app->compositor() ? app->compositor()->defaultSeat() : nullptr;
    QWaylandKeyboard *keyboard = seat ? seat->keyboard() : nullptr;
    if (!keyboard)
        return;
    if (seat->keyboardFocus() != app->surface())
        app->takeFocus(seat);
    int code = 0;
    switch (qtKey) {
    case Qt::Key_Up: code = 103; break;
    case Qt::Key_Down: code = 108; break;
    case Qt::Key_Left: code = 105; break;
    case Qt::Key_Right: code = 106; break;
    case Qt::Key_Return: case Qt::Key_Enter: code = 28; break;
    case Qt::Key_Escape: code = 1; break;
    case Qt::Key_Space: code = 57; break;
    default: break;
    }
    if (!code && text.size() == 1) {
        code = usKey(text[0]).first;
        if (!code)
            code = ruKey(text[0]).first;
    }
    if (!code && qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)   // буква без текста (с Ctrl или Alt)
        code = usKey(QChar(qtKey)).first;
    if (!code)
        return;
    down ? keyboard->sendKeyPressEvent(uint(code + 8)) : keyboard->sendKeyReleaseEvent(uint(code + 8));
}

// Клавиша Qt → код клавиши (evdev) английской клавиатуры: особые (Enter, стрелки, F1…F12) и буквы, цифры, знаки
// (клавиши Qt для них — коды ASCII). Нет такой — 0. Для программ Linux (keyToAndroid, textCommit)
static int linuxKey(int qtKey)
{
    static const struct { int key, code; } kSpecial[] = {
        {Qt::Key_Return, 28}, {Qt::Key_Enter, 28}, {Qt::Key_Tab, 15}, {Qt::Key_Backtab, 15}, {Qt::Key_Escape, 1},
        {Qt::Key_Backspace, 14}, {Qt::Key_Delete, 111}, {Qt::Key_Insert, 110}, {Qt::Key_Home, 102},
        {Qt::Key_End, 107}, {Qt::Key_PageUp, 104}, {Qt::Key_PageDown, 109}, {Qt::Key_Left, 105}, {Qt::Key_Up, 103},
        {Qt::Key_Right, 106}, {Qt::Key_Down, 108}, {Qt::Key_Space, 57}, {Qt::Key_F11, 87}, {Qt::Key_F12, 88}};
    for (const auto &k : kSpecial)
        if (qtKey == k.key)
            return k.code;
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F10)
        return 59 + (qtKey - Qt::Key_F1);
    if (qtKey > 0 && qtKey < 0x80)
        return usKey(QChar(qtKey).toLower()).first;
    return 0;
}

// Клавиша окну Android. Waydroid понимает клавиатуру только нажатиями клавиш (коды evdev): состояние модификаторов
// wl_keyboard.modifiers он не читает, а символ выбирает раскладка физической клавиатуры в самом Android (английская).
// Зритель же шлёт буквы готовым текстом (приставка — уже с Shift, Caps и своей раскладкой, без отдельных Shift),
// поэтому: символ английской клавиатуры — нажатием его клавиши, с Shift, если он нужен («F» — Shift+F; проверено
// 18 ноября 2026: без этого «F» печаталась «f»); русская буква — клавишей ЙЦУКЕН с правым Alt: у клавиатуры Waydroid
// своя раскладка Android, русские на правом Alt (wayland_keyboard.kcm, tools/naomi android tune), — так раскладку в
// Android переключать не нужно; сочетания и особые клавиши (Ctrl+C, Enter, стрелки) — событием Qt элементу окна
// (Qt Wayland найдёт код по клавише Qt), а их Shift, Ctrl, Alt, Win — нажатиями их клавиш, как держат у зрителя.
void StreamInput::keyToAndroid(QWaylandQuickItem *app, bool down, int qtKey, int modifiers, const QString &text)
{
    QWaylandSeat *seat = app->compositor() ? app->compositor()->defaultSeat() : nullptr;
    QWaylandKeyboard *keyboard = seat ? seat->keyboard() : nullptr;
    if (!keyboard)
        return;
    if (seat->keyboardFocus() != app->surface())
        app->takeFocus(seat);
    static const struct { Qt::KeyboardModifier mod; int key; uint code; } kMods[] = {
        {Qt::ShiftModifier, Qt::Key_Shift, 42 + 8}, {Qt::ControlModifier, Qt::Key_Control, 29 + 8},
        {Qt::AltModifier, Qt::Key_Alt, 56 + 8}, {Qt::MetaModifier, Qt::Key_Meta, 125 + 8}};
    // Программа Linux (свойство textCommit — окно Claude, docs/CLAUDE.md): модификаторы она знает только по
    // wl_keyboard.modifiers, а sendKeyPressEvent их не шлёт (см. holdModifiers) — Shift к ней не доходил
    // («Test» печаталось «test»). Ей — и состояние модификаторов. Своей русской раскладки у неё нет (раскладка
    // композитора — английская): буква не с английской клавиатуры — текстом, QInputMethodEvent окну, Qt отдаёт его
    // программе по text-input-v3 (Main.qml, TextInputManagerV3)
    bool commit = false;
    for (QQuickItem *i = app; i && !commit; i = i->parentItem())
        commit = i->property("textCommit").toBool();
    auto hold = [&](const auto &m, bool on) {
        if (bool(m_androidMods & m.mod) == on)
            return;
        on ? keyboard->sendKeyPressEvent(m.code) : keyboard->sendKeyReleaseEvent(m.code);
        if (commit)
            QWaylandKeyboardPrivate::get(keyboard)->updateModifierState(
                m.code, on ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        m_androidMods.setFlag(m.mod, on);
    };
    for (const auto &m : kMods) {
        if (qtKey == m.key) {   // сам модификатор (окно Mac шлёт и их)
            hold(m, down);
            return;
        }
    }
    const bool combo = modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (commit && !combo && !text.isEmpty() && text[0].isPrint() && !usKey(text[0]).first) {
        if (down) {
            for (const auto &m : kMods)
                hold(m, false);
            QInputMethodEvent e;
            e.setCommitString(text);
            QCoreApplication::sendEvent(app, &e);
        }
        return;
    }
    if (!combo && text.size() == 1 && text[0].isPrint()) {
        auto [code, shift] = usKey(text[0]);
        bool ru = false;
        if (!code) {
            std::tie(code, shift) = ruKey(text[0]);
            ru = code != 0;
        }
        if (!code)
            return;   // ни английской, ни русской клавиатуры
        constexpr uint kRightAlt = 100 + 8;
        if (down) {
            // буква без Ctrl, Alt и Win: если они остались зажатыми от прошлого сочетания (его отпускание пришло
            // с теми же модификаторами — так шлёт проверочный ввод), — отпустить, иначе буква вышла бы сочетанием
            for (size_t i = 1; i < std::size(kMods); ++i)
                hold(kMods[i], false);
            hold(kMods[0], shift);
            if (ru)
                keyboard->sendKeyPressEvent(kRightAlt);
            keyboard->sendKeyPressEvent(code + 8);
        } else {
            keyboard->sendKeyReleaseEvent(code + 8);
            if (ru)
                keyboard->sendKeyReleaseEvent(kRightAlt);
            hold(kMods[0], false);   // Shift — только на время символа: иначе щелчок мышью вышел бы с Shift
        }
        return;
    }
    for (const auto &m : kMods)
        hold(m, modifiers & m.mod);
    if (!qtKey && text.size() == 1 && text[0].unicode() < 0x80)
        qtKey = text[0].toUpper().unicode();
    // Программе Linux — клавишей Wayland напрямую: QKeyEvent без кода клавиши Qt Wayland отдаёт полю с включённым
    // text-input-v3 текстом, а не клавишей (QWaylandSeat::sendFullKeyEvent) — Enter в поле Claude терялся
    if (commit) {
        if (const int code = linuxKey(qtKey))
            down ? keyboard->sendKeyPressEvent(code + 8) : keyboard->sendKeyReleaseEvent(code + 8);
        return;
    }
    QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, Qt::KeyboardModifiers(modifiers), text);
    QCoreApplication::sendEvent(app, &e);
}

bool StreamInput::acceptsText(QObject *focus)
{
    QInputMethodQueryEvent q(Qt::ImEnabled);
    QCoreApplication::sendEvent(focus, &q);
    return q.value(Qt::ImEnabled).toBool();
}

// «назад», «домой», шторка — те же жесты, что пальцем: палец идёт от края за 0,15–0,2 с
void StreamInput::nav(int action)
{
    if (!m_window || m_desktop || m_gestureTimer.isActive())
        return;
    const QRect g = m_window->screen()->handle()->geometry();
    const double dp = m_window->devicePixelRatio();
    const double w = g.width(), h = g.height();
    int ms = 150;
    switch (action) {
    case stream::NavBack:
        m_gFrom = {2, h * 0.5};
        m_gTo = {2 + 200 * dp, h * 0.5};
        break;
    case stream::NavHome:
        m_gFrom = {w * 0.5, h - 2};
        m_gTo = {w * 0.5, h - 2 - 400 * dp};
        ms = 180;
        break;
    case stream::NavShade:
        m_gFrom = {w * 0.5, 20 * dp};
        m_gTo = {w * 0.5, 520 * dp};
        ms = 200;
        break;
    default:
        return;
    }
    m_gStep = 0;
    m_gSteps = std::max(2, ms / 8);
    sendTouch(9, int(QEventPoint::State::Pressed), m_gFrom);
    m_gestureTimer.start();
}

void StreamInput::releaseAll()
{
    const auto ids = m_down.keys();
    for (int id : ids)
        sendTouch(id, int(QEventPoint::State::Released), m_down.value(id));
    m_gestureTimer.stop();
    if (m_scrolling)
        scroll(0, 0, 0, 0, stream::ScrollEnd);
    m_shotKeys.clear();
    m_cmdKeys.clear();
}
