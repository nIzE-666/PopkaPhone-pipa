#include "term_view.h"

#include "term_link.h"

#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QTextLayout>
#include <QTouchEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kBlinkMs = 530;        // как каретка стола (CARET_BLINK_MS)
constexpr int kBlinkForMs = 10000;   // мигать столько после последнего нажатия или вывода

// что нарисовано в строке: правили строку, курсор или выделение по ней прошли — собрать заново
struct RowKey {
    quint32 version = ~0u;
    int cursor = -1;
    int cursorStyle = 0;   // 1 — блок, 2 — рамка (окно не в фокусе), 3 — черта снизу, 4 — черта слева
    int selFrom = 0, selTo = 0;
    int cols = 0;
    int gen = -1;
    bool operator==(const RowKey &o) const
    {
        return version == o.version && cursor == o.cursor && cursorStyle == o.cursorStyle && selFrom == o.selFrom
            && selTo == o.selTo && cols == o.cols && gen == o.gen;
    }
};

class RowNode : public QSGTransformNode
{
public:
    RowKey key;
    bool used = false;
};

class RootNode : public QSGNode
{
public:
    QHash<quint64, RowNode *> rows;
    void clearRows()
    {
        for (RowNode *n : std::as_const(rows)) {
            removeChildNode(n);
            delete n;
        }
        rows.clear();
    }
};

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QSGTextNode::RenderType renderType()
{
    switch (QQuickWindow::textRenderType()) {
    case QQuickWindow::NativeTextRendering:
        return QSGTextNode::NativeRendering;
    case QQuickWindow::CurveTextRendering:
        return QSGTextNode::CurveRendering;
    default:
        return QSGTextNode::QtRendering;
    }
}

}   // namespace

TermView::TermView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setFlag(ItemAcceptsInputMethod);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptTouchEvents(true);
    setClip(true);
    setCursor(Qt::IBeamCursor);

    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(450);
    connect(&m_longPressTimer, &QTimer::timeout, this, [this] {
        if (m_touchId >= 0 && !m_touchScrolled) {
            m_touchLongFired = true;
            emit menuRequested(m_touchStart.x(), m_touchStart.y());
        }
    });

    // палитра — тона стола (DeskStyle): 16 своих, дальше — куб 6 × 6 × 6 и серые, как у xterm
    m_fg = QColor(0xec, 0xe9, 0xe3);       // DeskStyle.text
    m_bg = QColor(0x0e, 0x0f, 0x0c);       // DeskStyle.bg — фон окна
    m_cursor = QColor(0xd8, 0xb2, 0x5a);   // янтарь
    m_sel = mix(m_bg, m_cursor, 0.35);     // выделение — янтарь .35, как в строках ввода стола
    static const QRgb base16[16] = {0x2a2a26, 0xd9705f, 0x7fc98f, 0xd8b25a, 0x7aa2d6, 0xc08ad0, 0x63b6b0, 0xc9c4ba,
                                    0x7d786e, 0xeb8e7e, 0xa2dcaf, 0xecc978, 0x9dbde6, 0xd7a9e3, 0x8ad0ca, 0xf4f1ec};
    for (int i = 0; i < 16; ++i)
        m_palette[i] = QColor(base16[i]);
    static const int level[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; ++i)
        m_palette[16 + i] = QColor(level[i / 36], level[i / 6 % 6], level[i % 6]);
    for (int i = 0; i < 24; ++i)
        m_palette[232 + i] = QColor(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);

    m_font.setFamily(QStringLiteral("Ubuntu Sans Mono"));
    m_font.setPixelSize(14);
    measure();

    m_blinkTimer.setInterval(kBlinkMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        if (!m_lastActivity.isValid() || m_lastActivity.elapsed() >= kBlinkForMs) {
            m_blinkTimer.stop();
            m_phase = true;   // дальше горит ровно
        } else {
            m_phase = !m_phase;
        }
        update();
    });
}

TermSession *TermView::s() const { return m_session.data(); }

QObject *TermView::session() const { return m_session.data(); }

void TermView::setSession(QObject *session)
{
    auto *ses = qobject_cast<TermSession *>(session);
    if (ses == m_session)
        return;
    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);
    m_session = ses;
    if (ses)
        connect(ses, &TermSession::changed, this, &TermView::changed);
    m_hasSel = false;
    m_selecting = false;
    m_lastAlt = ses && ses->screen().altScreen();
    ++m_gen;
    emit selectionChanged();
    emit sessionChanged();
    activity();
}

void TermView::setFontFamily(const QString &family)
{
    if (family == m_font.family())
        return;
    m_font.setFamily(family);
    measure();
}

void TermView::setFontSize(qreal size)
{
    const int px = qRound(size);
    if (px <= 0 || px == m_font.pixelSize())
        return;
    m_font.setPixelSize(px);
    measure();
}

void TermView::setPixel(qreal pixel)
{
    if (pixel <= 0 || qFuzzyCompare(pixel, m_pixel))
        return;
    m_pixel = pixel;
    emit pixelChanged();
    update();
}

void TermView::setBlink(bool on)
{
    if (on == m_blink)
        return;
    m_blink = on;
    emit blinkChanged();
    if (on)
        activity();
    else
        updateBlink();
}

void TermView::measure()
{
    m_font.setKerning(false);
    m_font.setHintingPreference(QFont::PreferNoHinting);   // ширина знака — как у глифов в строке, без округлений
    const QFontMetricsF fm(m_font);
    m_cellW = fm.horizontalAdvance(QLatin1Char('M'));
    const qreal text = fm.ascent() + fm.descent();
    m_cellH = std::max<qreal>(std::ceil(text), std::round(m_font.pixelSize() * 1.3));
    m_baseline = (m_cellH - text) / 2 + fm.ascent();
    m_inFont.clear();
    ++m_gen;
    emit fontChanged();
    relayout();
    update();
}

void TermView::relayout()
{
    const int cols = std::max(2, int((width() - 2 * kPadX) / m_cellW));
    const int rows = std::max(1, int((height() - 2 * kPadY) / m_cellH));
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    emit gridChanged();
    update();
}

void TermView::geometryChange(const QRectF &now, const QRectF &before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size())
        relayout();
}

void TermView::changed()
{
    if (TermSession *ses = s()) {
        const bool alt = ses->screen().altScreen();
        if (alt != m_lastAlt) {   // запасной экран включили или выключили — выделение больше ни к чему
            m_lastAlt = alt;
            clearSelection();
        }
    }
    activity();
}

void TermView::activity()
{
    m_lastActivity.restart();
    m_phase = true;
    updateBlink();
    update();
}

void TermView::updateBlink()
{
    const bool want = m_blink && hasActiveFocus() && s();
    if (want && !m_blinkTimer.isActive())
        m_blinkTimer.start();
    else if (!want && m_blinkTimer.isActive())
        m_blinkTimer.stop();
    if (!want && !m_phase) {
        m_phase = true;
        update();
    }
}

void TermView::focusInEvent(QFocusEvent *e)
{
    QQuickItem::focusInEvent(e);
    activity();
}

void TermView::focusOutEvent(QFocusEvent *e)
{
    QQuickItem::focusOutEvent(e);
    updateBlink();
    update();
}

// ---------- прокрутка ----------
qint64 TermView::base() const
{
    TermSession *ses = s();
    return ses && !ses->screen().altScreen() ? qint64(ses->screen().dropped()) : 0;
}

double TermView::visibleTop() const
{
    TermSession *ses = s();
    if (!ses)
        return 0;
    const term::Screen &sc = ses->screen();
    const int hist = sc.history();
    if (sc.altScreen() || ses->anchor < 0)
        return hist;
    return std::clamp(ses->anchor - double(base()), 0.0, double(hist));
}

void TermView::scrollLines(double lines)
{
    TermSession *ses = s();
    if (!ses)
        return;
    const int hist = ses->screen().history();
    const double t = visibleTop() + lines;
    ses->anchor = t >= hist - 1e-6 ? -1 : std::max(0.0, t) + double(base());
    update();
}

void TermView::scrollToBottom()
{
    if (TermSession *ses = s(); ses && ses->anchor >= 0) {
        ses->anchor = -1;
        update();
    }
}

// ---------- отрисовка ----------
QColor TermView::color(uint32_t c, bool fg) const
{
    if (c == term::kDefault)
        return fg ? m_fg : m_bg;
    if (c & 0x02000000u)
        return QColor((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    return m_palette[c & 0xff];
}

QSGNode *TermView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *root = static_cast<RootNode *>(old);
    if (!root)
        root = new RootNode;
    TermSession *ses = s();
    if (!ses || m_cols <= 0 || m_rows <= 0 || !window()) {
        root->clearRows();
        return root;
    }
    const term::Screen &sc = ses->screen();
    const int count = sc.lineCount();
    const double top = visibleTop();
    const int first = std::max(0, int(std::floor(top)));
    const int last = std::min(count, int(std::ceil(top + m_rows)));
    const int cursorLine = sc.history() + sc.cursorRow();
    const Pos selA = std::min(m_selA, m_selB), selB = std::max(m_selA, m_selB);
    const qint64 bs = base();
    const int shape = sc.cursorShape();

    for (RowNode *n : std::as_const(root->rows))
        n->used = false;
    for (int i = first; i < last; ++i) {
        const term::Line &line = sc.line(i);
        RowKey key;
        key.version = line.version;
        key.cols = m_cols;
        key.gen = m_gen;
        if (i == cursorLine && sc.cursorVisible()) {
            key.cursorStyle = !hasActiveFocus() ? 2 : !m_phase ? 0 : shape == 3 || shape == 4 ? 3 : shape >= 5 ? 4 : 1;
            key.cursor = key.cursorStyle ? sc.cursorCol() : -1;
        }
        if (m_hasSel) {
            const qint64 l = i + bs;
            if (l >= selA.line && l <= selB.line) {
                key.selFrom = l == selA.line ? selA.col : 0;
                key.selTo = l == selB.line ? selB.col : m_cols;
            }
        }
        RowNode *n = root->rows.value(line.uid);
        if (!n) {
            n = new RowNode;
            root->appendChildNode(n);
            root->rows.insert(line.uid, n);
        }
        if (!(n->key == key)) {
            buildRow(n, line, key.cursor, key.cursorStyle, key.selFrom, key.selTo);
            n->key = key;
        }
        n->used = true;
        const double y = kPadY + (i - top) * m_cellH;
        QMatrix4x4 m;
        m.translate(float(kPadX), float(std::round(y * m_pixel) / m_pixel));
        n->setMatrix(m);
    }
    for (auto it = root->rows.begin(); it != root->rows.end();) {
        if (it.value()->used) {
            ++it;
            continue;
        }
        root->removeChildNode(it.value());
        delete it.value();
        it = root->rows.erase(it);
    }
    return root;
}


bool TermView::simple(char32_t c) const
{
    if (c < 0x7f)
        return true;
    if (term::charWidth(c) != 1)
        return false;
    auto it = m_inFont.constFind(c);
    if (it == m_inFont.constEnd())
        it = m_inFont.insert(c, QFontMetricsF(m_font).inFontUcs4(uint(c)));
    return it.value();
}

// Строка: фон кусками одного цвета, текст — вставками QTextLayout в один узел текста (обычные знаки подряд — одной
// вставкой с цветами по кускам; знак не из шрифта или широкий — своей, точно на своей клетке), потом курсор-рамка
void TermView::buildRow(QSGNode *node, const term::Line &line, int cursor, int cursorStyle, int selFrom, int selTo)
{
    while (QSGNode *c = node->firstChild()) {
        node->removeChildNode(c);
        delete c;
    }
    QQuickWindow *win = window();
    const int cols = m_cols;
    const int have = int(line.cells.size());
    struct Eff {
        QColor fg, bg;
        bool hasBg = false;
        uint16_t attr = 0;
        char32_t ch = U' ';
    };
    std::vector<Eff> eff(static_cast<size_t>(cols));
    for (int x = 0; x < cols; ++x) {
        const term::Cell c = x < have ? line.cells[size_t(x)] : term::Cell {};
        Eff &e = eff[size_t(x)];
        e.fg = color(c.fg, true);
        e.bg = color(c.bg, false);
        e.hasBg = c.bg != term::kDefault;
        e.attr = c.attr;
        e.ch = c.ch;
        if (c.attr & term::Inverse) {
            std::swap(e.fg, e.bg);
            e.hasBg = true;
        }
        if (c.attr & term::Faint)
            e.fg = mix(e.bg, e.fg, 0.6);
        if (x >= selFrom && x < selTo) {
            e.bg = mix(e.hasBg ? e.bg : m_bg, m_cursor, 0.35);
            e.hasBg = true;
        }
        if (x == cursor && cursorStyle == 1) {
            e.bg = m_cursor;
            e.fg = m_bg;
            e.hasBg = true;
        }
        if (c.attr & term::Hidden)
            e.fg = e.bg;
    }
    // широкий знак под курсором — курсор на обе клетки
    const int cursorW = cursor >= 0 && cursor + 1 < cols && cursor + 1 < have && line.cells[size_t(cursor) + 1].ch == 0 ? 2 : 1;
    if (cursorStyle == 1 && cursorW == 2) {
        eff[size_t(cursor) + 1].bg = m_cursor;
        eff[size_t(cursor) + 1].hasBg = true;
    }

    const auto rect = [&](qreal x, qreal y, qreal w, qreal h, const QColor &c) {
        QSGRectangleNode *r = win->createRectangleNode();
        r->setRect(QRectF(x, y, w, h));
        r->setColor(c);
        node->appendChildNode(r);
    };
    for (int x = 0; x < cols;) {
        if (!eff[size_t(x)].hasBg) {
            ++x;
            continue;
        }
        int e = x + 1;
        while (e < cols && eff[size_t(e)].hasBg && eff[size_t(e)].bg == eff[size_t(x)].bg)
            ++e;
        rect(x * m_cellW, 0, (e - x) * m_cellW, m_cellH, eff[size_t(x)].bg);
        x = e;
    }

    QSGTextNode *text = nullptr;
    const auto place = [&](int x0, const QString &str, const QList<QTextLayout::FormatRange> &formats) {
        QTextLayout layout(str, m_font);
        QTextOption opt;
        opt.setWrapMode(QTextOption::NoWrap);
        opt.setUseDesignMetrics(true);
        layout.setTextOption(opt);
        layout.setFormats(formats);
        layout.beginLayout();
        QTextLine tl = layout.createLine();
        if (!tl.isValid()) {
            layout.endLayout();
            return;
        }
        tl.setPosition(QPointF(0, 0));
        layout.endLayout();
        if (!text) {
            text = win->createTextNode();
            text->setColor(m_fg);
            text->setRenderType(renderType());
            node->appendChildNode(text);
        }
        text->addTextLayout(QPointF(x0 * m_cellW, m_baseline - tl.ascent()), &layout);
    };
    const auto format = [](const Eff &e) {
        QTextCharFormat f;
        f.setForeground(e.fg);
        if (e.attr & term::Bold)
            f.setFontWeight(QFont::Bold);
        if (e.attr & term::Italic)
            f.setFontItalic(true);
        if (e.attr & term::Underline)
            f.setFontUnderline(true);
        if (e.attr & term::Strike)
            f.setFontStrikeOut(true);
        return f;
    };
    const auto blankCell = [&](int x) {
        const Eff &e = eff[size_t(x)];
        return (e.ch == U' ' || e.ch == 0) && !(e.attr & (term::Underline | term::Strike));
    };
    const auto sameLook = [](const Eff &a, const Eff &b) {
        return a.fg == b.fg && (a.attr & ~term::Inverse) == (b.attr & ~term::Inverse);
    };
    int x = 0;
    while (x < cols) {
        const Eff &e = eff[size_t(x)];
        if (e.ch == 0 || blankCell(x)) {   // пустое и вторые половины широких — без текста
            ++x;
            continue;
        }
        if (!simple(e.ch)) {   // знак не из шрифта или широкий — своей вставкой
            QString one;
            if (e.ch > 0xffff) {
                one += QChar(QChar::highSurrogate(e.ch));
                one += QChar(QChar::lowSurrogate(e.ch));
            } else {
                one += QChar(char16_t(e.ch));
            }
            place(x, one, {{0, int(one.size()), format(e)}});
            ++x;
            continue;
        }
        // обычные знаки подряд (пробелы внутри — тоже), до необычного; хвост пробелов — прочь
        int end = x;
        while (end < cols && eff[size_t(end)].ch != 0 && simple(eff[size_t(end)].ch))
            ++end;
        while (end > x && blankCell(end - 1))
            --end;
        QString str;
        QList<QTextLayout::FormatRange> formats;
        for (int i = x; i < end; ++i) {
            const Eff &c = eff[size_t(i)];
            if (formats.isEmpty() || !sameLook(eff[size_t(i) - 1], c) || i == x)
                formats.append({int(str.size()), 0, format(c)});
            str += QChar(char16_t(c.ch));
            formats.last().length += 1;
        }
        place(x, str, formats);
        x = end;
    }

    if (cursor >= 0 && cursorStyle >= 2) {
        const qreal cx = cursor * m_cellW, cw = cursorW * m_cellW, t = 1 / m_pixel;
        if (cursorStyle == 2) {   // окно не в фокусе — рамка
            rect(cx, 0, cw, t, m_cursor);
            rect(cx, m_cellH - t, cw, t, m_cursor);
            rect(cx, 0, t, m_cellH, m_cursor);
            rect(cx + cw - t, 0, t, m_cellH, m_cursor);
        } else if (cursorStyle == 3) {
            rect(cx, m_cellH - 2, cw, 2, m_cursor);
        } else {
            rect(cx, 0, 2, m_cellH, m_cursor);
        }
    }
}

// ---------- клавиши ----------
QByteArray TermView::encodeKey(QKeyEvent *e) const
{
    TermSession *ses = s();
    if (!ses)
        return {};
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
    const int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    const bool app = ses->screen().appCursor();
    const auto cursorKey = [&](char final) {
        if (m > 1)
            return QByteArray("\x1b[1;") + QByteArray::number(m) + final;
        return QByteArray(app ? "\x1bO" : "\x1b[") + final;
    };
    const auto tilde = [&](int code) {
        return QByteArray("\x1b[") + QByteArray::number(code) + (m > 1 ? ";" + QByteArray::number(m) : QByteArray()) + '~';
    };
    const int key = e->key();
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return alt ? "\x1b\r" : "\r";
    case Qt::Key_Backspace:
        return ctrl ? "\x08" : alt ? "\x1b\x7f" : "\x7f";
    case Qt::Key_Tab:
        return shift ? "\x1b[Z" : "\t";
    case Qt::Key_Backtab:
        return "\x1b[Z";
    case Qt::Key_Escape:
        return "\x1b";
    case Qt::Key_Up: return cursorKey('A');
    case Qt::Key_Down: return cursorKey('B');
    case Qt::Key_Right: return cursorKey('C');
    case Qt::Key_Left: return cursorKey('D');
    case Qt::Key_Home: return cursorKey('H');
    case Qt::Key_End: return cursorKey('F');
    case Qt::Key_Insert: return tilde(2);
    case Qt::Key_Delete: return tilde(3);
    case Qt::Key_PageUp: return tilde(5);
    case Qt::Key_PageDown: return tilde(6);
    case Qt::Key_F1:
    case Qt::Key_F2:
    case Qt::Key_F3:
    case Qt::Key_F4: {
        const char final = char('P' + (key - Qt::Key_F1));
        return m > 1 ? QByteArray("\x1b[1;") + QByteArray::number(m) + final : QByteArray("\x1bO") + final;
    }
    case Qt::Key_F5: return tilde(15);
    case Qt::Key_F6: return tilde(17);
    case Qt::Key_F7: return tilde(18);
    case Qt::Key_F8: return tilde(19);
    case Qt::Key_F9: return tilde(20);
    case Qt::Key_F10: return tilde(21);
    case Qt::Key_F11: return tilde(23);
    case Qt::Key_F12: return tilde(24);
    default:
        break;
    }
    const QByteArray esc = alt ? QByteArray("\x1b") : QByteArray();
    if (ctrl) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return esc + char(key - Qt::Key_A + 1);
        switch (key) {
        case Qt::Key_Space: case Qt::Key_2: case Qt::Key_At: return esc + QByteArray(1, '\0');
        case Qt::Key_BracketLeft: case Qt::Key_3: return esc + "\x1b";
        case Qt::Key_Backslash: case Qt::Key_4: return esc + "\x1c";
        case Qt::Key_BracketRight: case Qt::Key_5: return esc + "\x1d";
        case Qt::Key_6: case Qt::Key_AsciiCircum: return esc + "\x1e";
        case Qt::Key_Minus: case Qt::Key_7: case Qt::Key_Slash: case Qt::Key_Underscore: return esc + "\x1f";
        case Qt::Key_8: case Qt::Key_Question: return esc + "\x7f";
        default: break;
        }
    }
    // Alt+знак: зритель шлёт клавишу латиницей без текста (приставка) или с текстом
    if (alt && !ctrl) {
        if (!e->text().isEmpty())
            return esc + e->text().toUtf8();
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return esc + char((shift ? 'A' : 'a') + (key - Qt::Key_A));
        if (key > 0x20 && key < 0x7f)
            return esc + char(key);
    }
    const QString text = e->text();
    if (!text.isEmpty() && !ctrl && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7f)
        return text.toUtf8();
    return {};
}

void TermView::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
    const int key = e->key();
    e->accept();
    // свои сочетания — как у браузера и у терминалов Linux
    if (ctrl && shift && key == Qt::Key_T)
        return emit shortcut(QStringLiteral("new"), 0);
    if (ctrl && shift && key == Qt::Key_W)
        return emit shortcut(QStringLiteral("close"), 0);
    if (ctrl && (key == Qt::Key_Backtab || (key == Qt::Key_Tab && shift)) )
        return emit shortcut(QStringLiteral("cycle"), -1);
    if (ctrl && key == Qt::Key_Tab)
        return emit shortcut(QStringLiteral("cycle"), 1);
    if (ctrl && !shift && key == Qt::Key_PageDown)
        return emit shortcut(QStringLiteral("cycle"), 1);
    if (ctrl && !shift && key == Qt::Key_PageUp)
        return emit shortcut(QStringLiteral("cycle"), -1);
    // копировать и вставить: Ctrl+Shift+C / V, как у терминалов Linux, и ⌘C / ⌘V, как у Mac (Ctrl+C — программе)
    const bool cmd = (mods & Qt::MetaModifier) && !ctrl && !alt;
    if ((ctrl && shift && key == Qt::Key_C) || (cmd && key == Qt::Key_C))
        return copy();
    if ((ctrl && shift && key == Qt::Key_V) || (cmd && key == Qt::Key_V))
        return paste();
    if (ctrl && (key == Qt::Key_Plus || key == Qt::Key_Equal))
        return emit shortcut(QStringLiteral("zoom"), 1);
    if (ctrl && shift && key == Qt::Key_Underscore)
        return emit shortcut(QStringLiteral("zoom"), -1);
    if (ctrl && !shift && key == Qt::Key_0)
        return emit shortcut(QStringLiteral("zoom"), 0);
    if (ctrl && !shift && !alt && key >= Qt::Key_1 && key <= Qt::Key_9)
        return emit shortcut(QStringLiteral("select"), key == Qt::Key_9 ? -1 : key - Qt::Key_1);
    if (shift && !ctrl && !alt && s() && !s()->screen().altScreen()) {
        if (key == Qt::Key_PageUp)
            return scrollLines(-(m_rows - 1));
        if (key == Qt::Key_PageDown)
            return scrollLines(m_rows - 1);
        if (key == Qt::Key_Home)
            return scrollLines(-1e9);
        if (key == Qt::Key_End)
            return scrollToBottom();
    }
    const QByteArray bytes = encodeKey(e);
    if (bytes.isEmpty()) {
        e->ignore();
        return;
    }
    send(bytes);
}

void TermView::inputMethodEvent(QInputMethodEvent *e)
{
    const QString text = e->commitString();
    e->accept();
    if (text.isEmpty())
        return;
    // несколько знаков или перевод строки — вставка (⌘V в окне Mac), один знак — нажатие
    if (text.size() > 2 || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
        sendPaste(text);
    else
        send(text.toUtf8());
}

QVariant TermView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    case Qt::ImCursorRectangle:
        if (TermSession *ses = s())
            return QRectF(kPadX + ses->screen().cursorCol() * m_cellW, kPadY + ses->screen().cursorRow() * m_cellH,
                          m_cellW, m_cellH);
        return QRectF();
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void TermView::send(const QByteArray &bytes)
{
    TermSession *ses = s();
    if (!ses || bytes.isEmpty())
        return;
    ses->input(bytes);
    scrollToBottom();
    activity();
}

void TermView::sendPaste(const QString &text)
{
    QByteArray d = text.toUtf8();
    d.replace("\r\n", "\r");
    d.replace('\n', '\r');
    if (s() && s()->screen().bracketedPaste())
        d = "\x1b[200~" + d + "\x1b[201~";
    send(d);
}

// ---------- выделение и буфер ----------
TermView::Pos TermView::posAt(QPointF p, bool round) const
{
    Pos out;
    TermSession *ses = s();
    if (!ses)
        return out;
    const int count = ses->screen().lineCount();
    const int i = std::clamp(int(std::floor(visibleTop() + (p.y() - kPadY) / m_cellH)), 0, std::max(0, count - 1));
    out.line = i + base();
    const double cx = (p.x() - kPadX) / m_cellW;
    out.col = std::clamp(int(round ? std::lround(cx) : std::floor(cx)), 0, m_cols);
    return out;
}

void TermView::clearSelection()
{
    if (!m_hasSel && !m_selecting)
        return;
    m_hasSel = false;
    m_selecting = false;
    emit selectionChanged();
    update();
}

static bool wordChar(char32_t c)
{
    if (c == 0)
        return true;
    if (c == U' ' || c == U'\t')
        return false;
    static const char32_t stop[] = U"\"'`()[]{}<>|;,";
    for (const char32_t *p = stop; *p; ++p)
        if (*p == c)
            return false;
    return true;
}

void TermView::selectWord(const Pos &p)
{
    TermSession *ses = s();
    if (!ses)
        return;
    const int i = int(p.line - base());
    if (i < 0 || i >= ses->screen().lineCount())
        return;
    const auto &cells = ses->screen().line(i).cells;
    const int n = std::min(int(cells.size()), m_cols);
    int a = std::min(p.col, n - 1), b = a;
    if (a < 0 || !wordChar(cells[size_t(a)].ch))
        return;
    while (a > 0 && wordChar(cells[size_t(a) - 1].ch))
        --a;
    while (b + 1 < n && wordChar(cells[size_t(b) + 1].ch))
        ++b;
    m_selA = {p.line, a};
    m_selB = {p.line, b + 1};
    m_hasSel = true;
    emit selectionChanged();
    update();
}

void TermView::selectLine(const Pos &p)
{
    m_selA = {p.line, 0};
    m_selB = {p.line, m_cols};
    m_hasSel = true;
    emit selectionChanged();
    update();
}

void TermView::copy()
{
    TermSession *ses = s();
    if (!ses || !m_hasSel)
        return;
    const term::Screen &sc = ses->screen();
    const Pos a = std::min(m_selA, m_selB), b = std::max(m_selA, m_selB);
    const qint64 bs = base();
    QString out;
    for (qint64 l = a.line; l <= b.line; ++l) {
        const int i = int(l - bs);
        if (i < 0 || i >= sc.lineCount())
            continue;
        QString t = sc.text(i, l == a.line ? a.col : 0, l == b.line ? b.col : 1 << 20);
        const bool joined = sc.line(i).wrapped && l != b.line;   // перенос по краю — одна строка
        if (!joined) {
            while (t.endsWith(QLatin1Char(' ')))
                t.chop(1);
        }
        out += t;
        if (!joined && l != b.line)
            out += QLatin1Char('\n');
    }
    if (!out.isEmpty())
        QGuiApplication::clipboard()->setText(out);
}

void TermView::paste()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty())
        sendPaste(text);
}

// ---------- мышь ----------
bool TermView::mouseToApp(QMouseEvent *e) const
{
    return s() && s()->screen().mouseMode() && !(e->modifiers() & Qt::ShiftModifier);
}

void TermView::reportMouse(int button, bool press, QPointF p, Qt::KeyboardModifiers mods, bool motion)
{
    TermSession *ses = s();
    if (!ses)
        return;
    const int col = std::clamp(int((p.x() - kPadX) / m_cellW) + 1, 1, m_cols);
    const int row = std::clamp(int((p.y() - kPadY) / m_cellH) + 1, 1, m_rows);
    int cb = button + (mods & Qt::ShiftModifier ? 4 : 0) + (mods & Qt::AltModifier ? 8 : 0)
           + (mods & Qt::ControlModifier ? 16 : 0) + (motion ? 32 : 0);
    if (ses->screen().mouseSgr()) {
        ses->input("\x1b[<" + QByteArray::number(cb) + ';' + QByteArray::number(col) + ';' + QByteArray::number(row)
                   + (press ? 'M' : 'm'));
        return;
    }
    if (!press)
        cb = 3 + (cb & ~3);
    if (col > 222 || row > 222)
        return;
    ses->input(QByteArray("\x1b[M") + char(32 + cb) + char(32 + col) + char(32 + row));
}

static int buttonCode(Qt::MouseButton b)
{
    return b == Qt::LeftButton ? 0 : b == Qt::MiddleButton ? 1 : b == Qt::RightButton ? 2 : -1;
}

void TermView::mousePressEvent(QMouseEvent *e)
{
    forceActiveFocus(Qt::MouseFocusReason);
    e->accept();
    if (mouseToApp(e)) {
        const int code = buttonCode(e->button());
        if (code >= 0) {
            m_mouseButton = code;
            reportMouse(code, true, e->position(), e->modifiers());
        }
        return;
    }
    if (e->button() == Qt::RightButton) {
        emit menuRequested(e->position().x(), e->position().y());
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        paste();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const Pos p = posAt(e->position(), true);
    if (m_doubleAt.isValid() && m_doubleAt.elapsed() < 400) {   // третий щелчок — строка
        m_doubleAt.invalidate();
        selectLine(posAt(e->position(), false));
        return;
    }
    m_selA = m_selB = p;
    m_selecting = true;
    if (m_hasSel) {
        m_hasSel = false;
        emit selectionChanged();
        update();
    }
}

void TermView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_mouseButton >= 0) {
        if (s() && s()->screen().mouseMode() >= 1002)
            reportMouse(m_mouseButton, true, e->position(), e->modifiers(), true);
        return;
    }
    if (!m_selecting)
        return;
    m_selB = posAt(e->position(), true);
    const bool has = !(m_selA == m_selB);
    if (has != m_hasSel) {
        m_hasSel = has;
        emit selectionChanged();
    }
    update();
}

void TermView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_mouseButton >= 0) {
        reportMouse(m_mouseButton, false, e->position(), e->modifiers());
        m_mouseButton = -1;
        return;
    }
    m_selecting = false;
}

void TermView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (mouseToApp(e) || e->button() != Qt::LeftButton) {
        mousePressEvent(e);
        return;
    }
    m_selecting = false;
    m_doubleAt.start();
    selectWord(posAt(e->position(), false));
}

void TermView::wheelEvent(QWheelEvent *e)
{
    TermSession *ses = s();
    e->accept();
    if (!ses)
        return;
    const double px = !e->pixelDelta().isNull() ? e->pixelDelta().y() : e->angleDelta().y() / 120.0 * 3 * m_cellH;
    const term::Screen &sc = ses->screen();
    const bool app = sc.mouseMode() && !(e->modifiers() & Qt::ShiftModifier);
    if (app || (sc.altScreen() && sc.alternateScroll())) {
        // программе: колесо мыши (64 — вверх, 65 — вниз) или стрелки — на каждую строку сдвига
        if (std::signbit(px) != std::signbit(m_wheel))
            m_wheel = 0;
        m_wheel += px;
        while (std::abs(m_wheel) >= m_cellH) {
            const bool up = m_wheel > 0;
            if (app)
                reportMouse(up ? 64 : 65, true, e->position(), e->modifiers());
            else
                ses->input(sc.appCursor() ? (up ? "\x1bOA" : "\x1bOB") : (up ? "\x1b[A" : "\x1b[B"));
            m_wheel -= up ? m_cellH : -m_cellH;
        }
        return;
    }
    scrollLines(-px / m_cellH);
}

void TermView::resetTouch()
{
    m_touchId = -1;
    m_touchScrolled = false;
    m_touchLongFired = false;
    m_touchAltAcc = 0;
    m_pinchDist = 0;
    m_longPressTimer.stop();
}

void TermView::touchUngrabEvent()
{
    resetTouch();
    QQuickItem::touchUngrabEvent();
}

void TermView::touchEvent(QTouchEvent *e)
{
    const auto type = e->type();
    if (type == QEvent::TouchCancel) {
        resetTouch();
        e->accept();
        return;
    }
    const auto &points = e->points();
    if (points.size() >= 2) {
        m_longPressTimer.stop();
        m_touchScrolled = true;
        const QPointF d = points[0].position() - points[1].position();
        const qreal dist = std::hypot(d.x(), d.y());
        if (m_pinchDist > 0) {
            if (dist - m_pinchDist > 26) {
                emit shortcut(QStringLiteral("zoom"), 1);
                m_pinchDist = dist;
            } else if (m_pinchDist - dist > 26) {
                emit shortcut(QStringLiteral("zoom"), -1);
                m_pinchDist = dist;
            }
        } else {
            m_pinchDist = dist;
        }
        e->accept();
        return;
    }
    m_pinchDist = 0;

    if (type == QEvent::TouchBegin) {
        resetTouch();
        if (points.isEmpty()) {
            e->ignore();
            return;
        }
        forceActiveFocus(Qt::MouseFocusReason);
        const auto &p = points.constFirst();
        m_touchId = p.id();
        m_touchStart = m_touchLast = p.position();
        m_longPressTimer.start();
        e->accept();
        return;
    }

    if (m_touchId < 0) {
        e->ignore();
        return;
    }
    e->accept();
    const auto it = std::find_if(points.cbegin(), points.cend(),
                                 [this](const QEventPoint &pt) { return pt.id() == m_touchId; });
    if (it == points.cend()) {
        if (type == QEvent::TouchEnd)
            resetTouch();
        return;
    }

    const QPointF pos = it->position();
    const QPointF fromStart = pos - m_touchStart;
    if (!m_touchScrolled && QPointF::dotProduct(fromStart, fromStart) > 64.0) {
        m_touchScrolled = true;
        m_longPressTimer.stop();
    }

    if (type == QEvent::TouchEnd || it->state() == QEventPoint::State::Released) {
        if (!m_touchScrolled && !m_touchLongFired) {
            if (m_hasSel)
                clearSelection();
            else if (s() && s()->screen().mouseMode()) {
                reportMouse(0, true, m_touchStart, Qt::NoModifier);
                reportMouse(0, false, m_touchStart, Qt::NoModifier);
            }
        }
        resetTouch();
        return;
    }

    const qreal dy = pos.y() - m_touchLast.y();
    m_touchLast = pos;
    if (!m_touchScrolled || qFuzzyIsNull(dy) || !s())
        return;

    TermSession *ses = s();
    const term::Screen &sc = ses->screen();
    if (sc.mouseMode() || (sc.altScreen() && sc.alternateScroll())) {
        m_touchAltAcc += dy;
        while (std::abs(m_touchAltAcc) >= m_cellH) {
            const bool up = m_touchAltAcc > 0;
            if (sc.mouseMode())
                reportMouse(up ? 64 : 65, true, m_touchStart, Qt::NoModifier);
            else
                ses->input(sc.appCursor() ? (up ? "\x1bOA" : "\x1bOB") : (up ? "\x1b[A" : "\x1b[B"));
            m_touchAltAcc -= up ? m_cellH : -m_cellH;
        }
    } else {
        scrollLines(-dy / m_cellH);
    }
}
