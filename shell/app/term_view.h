// НаомиФон — текст вкладки «Терминала» на столе (docs/TERMINAL.md): рисует экран вкладки (term_screen.h) прямо в
// сцене стола и отдаёт ей клавиши, мышь и колесо. В QML — Naomi.Desk TermView.
//   Отрисовка лёгкая: строка — свой узел сцены (фон кусками, текст — QSGTextNode, тот же движок глифов, что у Text),
//   узел держится за номер строки (Line::uid) и пересобирается, только когда строку правили (или по ней прошли курсор
//   и выделение). Новые строки и прокрутка двигают готовые узлы. Курсор мигает, пока окно в фокусе и недавно
//   печатали или что-то выводилось (10 с), — потом горит ровно: стол без дела кадров не рисует.
//   Сетка — по шрифту: ширина знака моноширинного шрифта, высота строки — 1,3 размера. Знаки, которых в шрифте нет
//   (эмодзи, редкие значки), и широкие — каждый своей вставкой на своей клетке: строка не «плывёт».
//   Клавиши — как у xterm (стрелки, F1–F12, Ctrl, Alt, режим клавиш курсора); ввод текста любой раскладки — событием
//   ввода; вставка — скобками, если программа просила (2004). Выделение мышью (двойной щелчок — слово, тройной —
//   строка), Ctrl+Shift+C / V, средняя кнопка — вставить. Колесо — история; на запасном экране (less, vim) — стрелки;
//   программе, что просила мышь (1000/1002/1003), — нажатия и колесо, с Shift — выделение как обычно.
#pragma once

#include "term_screen.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

class QSGNode;
class TermSession;

class TermView : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged)
    Q_PROPERTY(qreal pixel READ pixel WRITE setPixel NOTIFY pixelChanged)    // пикселей кадра в точке стола
    Q_PROPERTY(bool blink READ blink WRITE setBlink NOTIFY blinkChanged)    // окно в фокусе и стол смотрят
    Q_PROPERTY(int cols READ cols NOTIFY gridChanged)
    Q_PROPERTY(int rows READ rows NOTIFY gridChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY fontChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY fontChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    // ⌘C / ⌘V понимает сам — копировать и вставить; ввод зрителя не делает из них Ctrl+C / V (StreamInput::key):
    // Ctrl+C здесь — прервать программу
    Q_PROPERTY(bool commandKeys READ commandKeys CONSTANT)
public:
    bool commandKeys() const { return true; }
    explicit TermView(QQuickItem *parent = nullptr);

    QObject *session() const;
    void setSession(QObject *session);
    QString fontFamily() const { return m_font.family(); }
    void setFontFamily(const QString &family);
    qreal fontSize() const { return m_font.pixelSize(); }
    void setFontSize(qreal size);
    qreal pixel() const { return m_pixel; }
    void setPixel(qreal pixel);
    bool blink() const { return m_blink; }
    void setBlink(bool on);
    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    qreal cellWidth() const { return m_cellW; }
    qreal cellHeight() const { return m_cellH; }
    bool hasSelection() const { return m_hasSel; }

    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void insert(const QString &text) { sendPaste(text); }   // как вставку: пути файлов, брошенных в окно
    Q_INVOKABLE void sendRaw(const QString &seq) { send(seq.toUtf8()); }   // спецклавиши сенсорной панели (Esc, Tab, Ctrl+C, стрелки)

    static constexpr qreal kPadX = 12;
    static constexpr qreal kPadY = 8;

signals:
    void sessionChanged();
    void fontChanged();
    void pixelChanged();
    void blinkChanged();
    void gridChanged();
    void selectionChanged();
    void shortcut(const QString &name, int arg);   // new, close, cycle (±1), select (номер, −1 — последняя)
    void menuRequested(qreal x, qreal y);          // правая кнопка: меню «Копировать / Вставить»

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void touchEvent(QTouchEvent *e) override;
    void touchUngrabEvent() override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    struct Pos {
        qint64 line = 0;   // строка от начала истории за всё время (Screen::dropped + номер)
        int col = 0;
        bool operator<(const Pos &o) const { return line < o.line || (line == o.line && col < o.col); }
        bool operator==(const Pos &o) const { return line == o.line && col == o.col; }
    };

    TermSession *s() const;
    void measure();
    void relayout();
    void changed();
    void activity();
    void updateBlink();
    double visibleTop() const;       // номер верхней видимой строки в буфере (дробный — плавная прокрутка)
    void scrollLines(double lines);
    void scrollToBottom();
    qint64 base() const;             // сколько прибавить к номеру строки буфера, чтобы вышел Pos::line
    Pos posAt(QPointF p, bool round) const;
    void selectWord(const Pos &p);
    void selectLine(const Pos &p);
    QByteArray encodeKey(QKeyEvent *e) const;
    void send(const QByteArray &bytes);
    void sendPaste(const QString &text);
    bool mouseToApp(QMouseEvent *e) const;
    void reportMouse(int button, bool press, QPointF p, Qt::KeyboardModifiers mods, bool motion = false);
    QColor color(uint32_t c, bool fg) const;
    bool simple(char32_t c) const;   // знак рисуется в общей вставке строки: есть в шрифте и шириной в клетку
    void buildRow(QSGNode *node, const term::Line &line, int cursor, int cursorStyle, int selFrom, int selTo);

    QPointer<TermSession> m_session;
    QFont m_font;
    int m_gen = 0;                    // шрифт или размер сменились — строки собрать заново
    qreal m_cellW = 7, m_cellH = 18, m_baseline = 13;
    qreal m_pixel = 1.5;
    int m_cols = 0, m_rows = 0;
    QColor m_fg, m_bg, m_cursor, m_sel;
    QColor m_palette[256];
    mutable QHash<char32_t, bool> m_inFont;

    bool m_blink = false;
    bool m_phase = true;              // курсор виден в этой фазе мигания
    QTimer m_blinkTimer;
    QElapsedTimer m_lastActivity;

    Pos m_selA, m_selB;
    bool m_hasSel = false;
    bool m_selecting = false;
    bool m_lastAlt = false;
    QElapsedTimer m_doubleAt;         // для тройного щелчка
    int m_mouseButton = -1;           // какая кнопка нажата у программы (режим мыши)
    double m_wheel = 0;

    // Сенсорное управление на планшете: свайп — прокрутка, удержание — меню, щипок — масштаб шрифта
    void resetTouch();
    int m_touchId = -1;
    QPointF m_touchStart;
    QPointF m_touchLast;
    bool m_touchScrolled = false;
    bool m_touchLongFired = false;
    double m_touchAltAcc = 0;
    qreal m_pinchDist = 0;
    QTimer m_longPressTimer;
};
