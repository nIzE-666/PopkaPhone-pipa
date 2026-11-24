// Мерки рабочего стола — из рабочего стола НаомиОС (bcdesk: ~/Desktop/NaomiOS/src/desk.h, docs/DESIGN.md там же),
// те же числа в точках стола. Палитра — общая с телефоном (Naomi.Ui Theme), кроме угля панели.
pragma Singleton
import QtQuick
import Naomi.Ui

QtObject {
    readonly property bool tablet: typeof naomiTablet !== "undefined" && naomiTablet
    readonly property color bg: "#0e0f0c"          // N_BG — уголь панели
    readonly property color surface: Theme.surface // N_SURFACE #161614 — всплывающее
    readonly property color text: Theme.text
    readonly property color muted: Theme.muted
    readonly property color dim: Theme.dim
    readonly property color amber: Theme.amber
    readonly property color teal: Theme.teal
    readonly property color bad: Theme.bad
    readonly property color good: Theme.good
    function amberA(a) { return Qt.rgba(216 / 255, 178 / 255, 90 / 255, a) }
    function badA(a) { return Qt.rgba(217 / 255, 112 / 255, 95 / 255, a) }
    // сила Wi-Fi (0…100, NetworkManager) — сколько из трёх дуг значка гореть: 60 и выше — все, 35 и выше — две
    function wifiArcs(strength) { return strength >= 60 ? 3 : strength >= 35 ? 2 : strength > 0 ? 1 : 0 }
    readonly property color unlit: Qt.rgba(236 / 255, 233 / 255, 227 / 255, 0.2)   // погасшие дуги Wi-Fi

    readonly property real line: 0.16        // N_LINE: линии и рамки — янтарь с этой прозрачностью
    readonly property int panelH: tablet ? 60 : 40
    readonly property int panelSide: 16      // PANEL_SIDE
    readonly property int appH: tablet ? 44 : 30
    readonly property real icon: 14          // N_ICON — значок с линией
    readonly property real iconLine: 1.5     // N_ICON_LINE
    readonly property int sepGap: tablet ? 32 : 20
    readonly property int itemPad: 12        // N_ITEM_PAD
    readonly property int itemGap: 2         // N_ITEM_GAP — между элементами одной группы
    readonly property int btnH: tablet ? 44 : 34
    readonly property int doubleClickMs: 400 // DOUBLE_CLICK_MS
    readonly property int caretBlinkMs: 530  // CARET_BLINK_MS
    readonly property real itemHover: 0.07   // N_ITEM_HOVER
    readonly property real itemPress: 0.12   // N_ITEM_PRESS
    readonly property real caps: 13          // N_CAPS — моно-капс шильдиков
    readonly property real barText: 14       // N_BAR_TEXT — надписи панели
    readonly property real barSpacing: 13 * 0.12
    readonly property int barTextPad: 10     // N_BAR_TEXT_PAD
    readonly property int padX: 16           // N_PAD_X — поля модуля
    readonly property int padY: 14           // N_PAD_Y
    readonly property int plateH: 18         // N_PLATE_H — строка шильдика
    readonly property int bracket: 13        // N_BRACKET — угловые скобки
    readonly property int popGap: 6          // POP_GAP — от панели до меню, и от края экрана
    readonly property int sysW: 360          // SYS_W — меню N
    readonly property int sysIconGap: 6      // SYS_ICON_GAP
    readonly property int tileGap: 6         // APP_TILE_GAP
    readonly property int gamesGap: 6        // GAMES_GAP — между модулями меню
    // края окон — за них тянут размер (EdgeGrip). У НаомиОС рамка 8 снаружи (WEB_EDGE) — пользователю мала
    readonly property int edgeOut: tablet ? 18 : 12
    readonly property int edgeIn: 4          // и внутри (окна стола; сверху — не до кнопок полосы, они с 5)
    readonly property int edgeCorner: 32     // угол ловится на столько вдоль сторон — вся дуга закругления (≤ 32)

    // браузер (bcdesk.c, полоса Naomi Web; docs/WEB.md)
    readonly property int webBarH: tablet ? 54 : 40
    readonly property int windowButton: tablet ? 44 : 30
    readonly property int webTabW: 200       // WEB_TAB_W — вкладка не шире
    readonly property int webTabMin: 56      // WEB_TAB_MIN — уже не ужимаем
    readonly property int webActiveMin: 320  // WEB_ACTIVE_MIN — строка адреса не уже
    readonly property int webHugPad: 25      // пустая вкладка: от конца «Поиск» до края строки — как от звезды до слова
    readonly property int webGrabGap: 18     // WEB_GRAB_GAP — пустое место перед кнопками: за него тянут
    readonly property int webDownloadW: 240  // WEB_DOWNLOAD_W
    readonly property int webGhostW: 40      // «+» и «Избранное»: 16 + 2 × N_ITEM_PAD
    readonly property int webFieldMinW: 200  // WEB_FIELD_MIN_W — у́же: нет звёздочки
    readonly property int webMenuW: 232      // WEB_MENU_W, WEB_BOOKMARKS_W
    readonly property int webBookmarksRows: 14
    readonly property int webSentMs: 1500    // WEB_SENT_MS
    readonly property int webLoadCycleMs: 1100   // WEB_LOAD_CYCLE_MS — бег отрезка «грузится»
    readonly property int webLoadSettleMs: 260   // WEB_LOAD_SETTLE_MS — растекается в черту
    readonly property size webMin: Qt.size(640, 400)   // WEB_MIN_W/H — окно с полосой

    // Углы модулей и окон — «непрерывные», как в bcdesk (module_corner, corner_path): четверть суперэллипса
    // |x|^5 + |y|^5 = 1, размах — не больше 32 точек и 35 % меньшей стороны (на глаз — круг ~20)
    function corner(w, h) { return Math.min(0.35 * Math.min(w, h), 32) }
    // контур прямоугольника с такими углами — точками (углы — по 16 шагов): для Shape (PathPolyline). Строит C++
    // (naomiDesk.outline: при растягивании окна — на каждом шаге для 2–3 фигур); JS — для проверки QML на Mac без
    // оболочки
    function outline(x, y, w, h, e) {
        if (typeof naomiDesk !== "undefined" && naomiDesk && naomiDesk.outline)
            return naomiDesk.outline(x, y, w, h, e)
        const corners = [
            { cx: x + w - e, cy: y + e, sx: 1, sy: -1, down: true }, { cx: x + w - e, cy: y + h - e, sx: 1, sy: 1, down: false },
            { cx: x + e, cy: y + h - e, sx: -1, sy: 1, down: true }, { cx: x + e, cy: y + e, sx: -1, sy: -1, down: false }]
        const pts = [Qt.point(x + e, y)]
        for (const c of corners)
            for (let j = 0; j <= 16; ++j) {
                const t = (c.down ? 16 - j : j) / 16 * Math.PI / 2
                pts.push(Qt.point(c.cx + c.sx * e * Math.pow(Math.max(Math.cos(t), 0), 0.4),
                                  c.cy + c.sy * e * Math.pow(Math.max(Math.sin(t), 0), 0.4)))
            }
        pts.push(pts[0])
        return pts
    }
    // то же, скруглены только верхние углы (полоса окна Android, docs/ANDROID.md, «Полоса окна и панель»)
    function outlineTop(x, y, w, h, e) {
        if (typeof naomiDesk !== "undefined" && naomiDesk && naomiDesk.outlineTop)
            return naomiDesk.outlineTop(x, y, w, h, e)
        const pts = []
        const arc = (cx, sx, down) => {
            for (let j = 0; j <= 16; ++j) {
                const t = (down ? 16 - j : j) / 16 * Math.PI / 2
                pts.push(Qt.point(cx + sx * e * Math.pow(Math.max(Math.cos(t), 0), 0.4),
                                  y + e - e * Math.pow(Math.max(Math.sin(t), 0), 0.4)))
            }
        }
        arc(x + w - e, 1, true)
        pts.push(Qt.point(x + w, y + h), Qt.point(x, y + h))
        arc(x + e, -1, false)
        pts.push(pts[0])
        return pts
    }

    // моно: в НаомиОС — Adwaita Mono, на телефоне его нет — Ubuntu Sans Mono
    readonly property string mono: Theme.mono
    readonly property string sans: Theme.sans
    readonly property string clock: Theme.clock   // цифры часов (Google Sans Flex) — как часы приставки без дока
}
