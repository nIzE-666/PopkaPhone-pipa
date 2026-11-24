// Значок линией — перенос значков рабочего стола НаомиОС (bcdesk.c, cairo) по тем же координатам: единицы значка,
// центр — (0, 0); дуги и кривые Безье — точками. Ставится центром: x, y — середина значка. unit — единицы → точки
// стола, line — толщина линии в точках при любом масштабе.
//   kind: n, bt, speaker, headphones, phone, force, gear, reboot, logout, magnet, gamepad, globe, folder, film,
//   android (своё: плитка ANDROID — голова робота Android с усиками), play (своё: Google Play — треугольник «пуск»
//   с гранями значка Play);
//   разделы «Настроек» — monitor, mouse, battery;
//   кнопки окон —
//   back, forward, close, minimize, maximize, restore, list, grid, up, down, star (fill — залить звезду); путь
//   трансляции — route-cable, route-wifi; Wi-Fi телефона — wifi (дуги — arcFrom, arcTo); браузер (docs/WEB.md) —
//   plus, panel (окно с залитой «головой»: «в панель» и «в окно»), pen (переименовать закладку), trash (удалить
//   закладку; та же корзина — на новой вкладке у «Часто»), загрузки —
//   dl-progress, dl-done, dl-failed; окно Bluetooth — search (лупа, высота 11,5); окно «Интернет» — lan (кабель),
//   lock (закрытая сеть); «Терминал» (docs/TERMINAL.md) — term (плитка TERMINAL: уголок и черта, выбор пользователя А1),
//   proc (уголок и черта поменьше — во вкладке работает программа); Claude (docs/CLAUDE.md) — claude (звёздочка из
//   восьми лучей, длинные и короткие через один: своё, временное — пока пользователь не выбрал)
import QtQuick
import QtQuick.Shapes

Item {
    id: g
    property string kind
    property color color
    property real unit: 1
    property real line: 1.5
    property bool slash: false        // «без звука»: косая черта с прорезью цвета cut (как DEST_OUT в bcdesk)
    property bool fill: false         // залить контур (звезда «в избранном»)
    property color cut: "#0e0f0c"
    // Wi-Fi — только дуги [arcFrom, arcTo) из трёх, изнутри наружу; точка — с первой: сила сигнала дугами (окно и
    // значок Wi-Fi рисуют горящие дуги и поверх них — тусклые остальные)
    property int arcFrom: 0
    property int arcTo: 3

    function pt(pts) { return pts.map(p => Qt.point(p[0] * unit, p[1] * unit)) }
    // дуга: центр, радиус, от a0 к a1 (радианы; по часовой — по возрастанию, ось y вниз, как у cairo)
    function arc(cx, cy, r, a0, a1, n) {
        const out = []
        for (let i = 0; i <= n; ++i) {
            const a = a0 + (a1 - a0) * i / n
            out.push([cx + r * Math.cos(a), cy + r * Math.sin(a)])
        }
        return out
    }
    // кубическая кривая Безье от p0 (её точка уже в пути) — n точек без первой
    function bez(p0, p1, p2, p3, n) {
        const out = []
        for (let i = 1; i <= n; ++i) {
            const t = i / n, u = 1 - t
            out.push([u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                      u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]])
        }
        return out
    }
    // значок пути: монитор, срезанный кругом D вокруг точки метки m, и сама метка (её линии — от m). Чертёж — от
    // середины экрана; весь значок сдвинут на ox, чтобы встать по центру своей ширины 18,75 (от точки метки до правой
    // стороны экрана)
    function route(mark) {
        const PI = Math.PI, mx = -10, my = 6.25, D = 8.25, ox = 1.625
        const ly = my - Math.sqrt(D * D - (mx + 7) * (mx + 7))         // где кончается левая сторона экрана: −1,435
        const bx = mx + Math.sqrt(D * D - (my - 2.75) * (my - 2.75))   // где начинается низ экрана: −2,529
        const screen = [[-7, ly]].concat(arc(-5.4, -4.65, 1.6, PI, PI * 1.5, 6), arc(5.4, -4.65, 1.6, -PI / 2, 0, 6),
                                         arc(5.4, 1.15, 1.6, 0, PI / 2, 6), [[bx, 2.75]])
        return [screen, [[0, 2.75], [0, 6.25]], [[-1.75, 6.25], [1.75, 6.25]]]
            .concat(mark.map(l => l.map(p => [mx + p[0], my + p[1]])))
            .map(l => l.map(p => [p[0] + ox, p[1]]))
    }
    function rrect(x, y, w, h, r) {
        const PI = Math.PI
        const pts = [].concat(arc(x + w - r, y + r, r, -PI / 2, 0, 6), arc(x + w - r, y + h - r, r, 0, PI / 2, 6),
                              arc(x + r, y + h - r, r, PI / 2, PI, 6), arc(x + r, y + r, r, PI, PI * 1.5, 6))
        pts.push(pts[0])
        return pts
    }

    // линии значка
    readonly property var paths: {
        const PI = Math.PI
        switch (kind) {
        case "n": {   // буква N: две стойки и диагональ, ширина — 5/6 высоты (draw_naomi_glyph)
            const hh = 6.25, hw = hh * 5 / 6
            return [[[-hw, hh], [-hw, -hh], [hw, hh], [hw, -hh]]]
        }
        case "bt": {  // руна Bluetooth (draw_bt_rune), s = (N_ICON − линия) / 2
            const s = 6.25, w = s * 0.55
            return [[[-w, -s * 0.5], [w, s * 0.5], [0, s], [0, -s], [w, -s * 0.5], [-w, s * 0.5]]]
        }
        case "speaker":   // колонки: корпус и басовик; пищалка — точкой (draw_sound_glyph)
            return [rrect(-5, -6.7, 10, 13.4, 1.8), arc(0, 2.0, 2.85, 0, 2 * PI, 28)]
        // выход звука «Телефон» (своё, НаомиФона; выбран пользователем 20.11.2026 — «две волны», 01 из десяти): корпус
        // высотой колонок и шириной 7,4 с чертой внизу, справа — две волны звука из середины его правой стороны (радиусы
        // 3,1 и 6,2). Весь значок по центру своей ширины (−6,8…6,8 — в месте наушников, панель не двигается)
        case "phone":
            return [rrect(-6.8, -6.7, 7.4, 13.4, 1.8), [[-4.45, 4.1], [-1.75, 4.1]],
                    arc(0.6, 0, 3.1, -PI * 40 / 180, PI * 40 / 180, 12), arc(0.6, 0, 6.2, -PI * 42 / 180, PI * 42 / 180, 18)]
        case "headphones":   // оголовье дугой и две чашки
            return [[[-6.8, 2.2]].concat(arc(0, 0.2, 6.8, PI, 2 * PI, 24), [[6.8, 2.2]]),
                    rrect(-7.7, 1.3, 3.3, 5.3, 1.2), rrect(4.4, 1.3, 3.3, 5.3, 1.2)]
        case "force": {   // окно с заголовком и крестиком (draw_force_glyph)
            const w = 15, h = 12.5, x = -w / 2, y = -h / 2, bar = 3.4, my = y + bar + (h - bar) / 2
            return [rrect(x, y, w, h, 1.6), [[x, y + bar], [x + w, y + bar]],
                    [[-2.2, my - 2.2], [2.2, my + 2.2]], [[2.2, my - 2.2], [-2.2, my + 2.2]]]
        }
        case "gear": {    // 8 зубцов и ступица (draw_gear_glyph); r — по вершинам зубцов
            const r = 6.25 * 1.1, inner = r * 0.74, oh = 0.17, ih = 0.27
            let pts = []
            for (let i = 0; i < 8; ++i) {
                const a = 2 * PI * i / 8 - PI / 2, next = 2 * PI * (i + 1) / 8 - PI / 2
                if (i === 0) pts.push([inner * Math.cos(a - ih), inner * Math.sin(a - ih)])
                pts.push([r * Math.cos(a - oh), r * Math.sin(a - oh)], [r * Math.cos(a + oh), r * Math.sin(a + oh)],
                         [inner * Math.cos(a + ih), inner * Math.sin(a + ih)])
                pts = pts.concat(arc(0, 0, inner, a + ih, next - ih, 4))
            }
            pts.push(pts[0])
            return [pts, arc(0, 0, r * 0.3, 0, 2 * PI, 16)]
        }
        case "reboot":    // дуга по часовой с зазором справа сверху; наконечник — заливкой (draw_reboot_glyph)
            return [arc(0, 0, 6.25, -40 * PI / 180, 215 * PI / 180, 30)]
        case "logout":    // закрыть сессию (своё, НаомиФона): проём двери, открытый вправо, и стрелка из него наружу
            return [[[1.5, -6], [-5, -6], [-5, 6], [1.5, 6]], [[-1.5, 0], [6.5, 0]], [[3.5, -3], [6.5, 0], [3.5, 3]]]
        case "magnet": {  // подкова концами вверх, на концах — полосы полюсов (APP_ICON_MAGNET)
            const t = 0.6
            return [[[-6, -6.5 + t], [-6, -0.5 + t]].concat(arc(0, -0.5 + t, 6, PI, 0, 16),
                     [[6, -6.5 + t], [2.4, -6.5 + t], [2.4, -0.5 + t]], arc(0, -0.5 + t, 2.4, 0, PI, 10),
                     [[-2.4, -6.5 + t], [-6, -6.5 + t]]),
                    [[-6, -3.6 + t], [-2.4, -3.6 + t]], [[2.4, -3.6 + t], [6, -3.6 + t]]]
        }
        case "gamepad": { // корпус с ручками и крестовина; кнопки — точками (draw_gamepad_glyph)
            const y = -0.5
            let b = [[-5, y - 5], [5, y - 5]]
            b = b.concat(bez([5, y - 5], [8.5, y - 5], [9.5, y - 2], [9.5, y + 1], 8))
            b = b.concat(bez([9.5, y + 1], [9.5, y + 4.5], [8.5, y + 6], [7, y + 6], 6))
            b = b.concat(bez([7, y + 6], [5.5, y + 6], [4.5, y + 4.5], [3, y + 3], 6))
            b.push([-3, y + 3])
            b = b.concat(bez([-3, y + 3], [-4.5, y + 4.5], [-5.5, y + 6], [-7, y + 6], 6))
            b = b.concat(bez([-7, y + 6], [-8.5, y + 6], [-9.5, y + 4.5], [-9.5, y + 1], 6))
            b = b.concat(bez([-9.5, y + 1], [-9.5, y - 2], [-8.5, y - 5], [-5, y - 5], 8))
            return [b, [[-6.8, y - 0.8], [-3.2, y - 0.8]], [[-5, y - 2.6], [-5, y + 1]]]
        }
        case "globe":     // круг, меридиан (сжатый круг), экватор и две параллели (APP_ICON_GLOBE)
            return [arc(0, 0, 7.2, 0, 2 * PI, 32), arc(0, 0, 7.2, 0, 2 * PI, 32).map(p => [p[0] * 0.42, p[1]]),
                    [[-7.2, 0], [7.2, 0]], [[-6.1, -3.6], [6.1, -3.6]], [[-6.1, 3.6], [6.1, 3.6]]]
        case "android": { // голова робота Android: полукруг на ровном низе, усики наружу по радиусу; глаза — точками
            const r = 7, cy = 4.1   // по высоте — от кончиков усиков (−4) до низа (4,1)
            const head = arc(0, cy, r, PI, 2 * PI, 24)
            head.push(head[0])
            const ant = a => [[r * Math.cos(a), cy + r * Math.sin(a)], [9.4 * Math.cos(a), cy + 9.4 * Math.sin(a)]]
            return [head, ant(PI * 4 / 3), ant(PI * 5 / 3)]
        }
        case "play":      // Google Play: треугольник «пуск», от левых углов — грани к противоположным сторонам
            return [[[-5, -6.5], [6.5, 0], [-5, 6.5], [-5, -6.5]], [[-5, -6.5], [2, 2.55]], [[-5, 6.5], [2, -2.55]]]
        case "monitor":   // экран (своё, НаомиФона: раздел «Экран» в «Настройках») — рамка и подставка, высотой N_ICON
            return [rrect(-6.5, -6, 13, 9, 1.5), [[-2.75, 6.25], [2.75, 6.25]], [[0, 3], [0, 6.25]]]
        case "mouse":     // мышь (своё: раздел «Мышь») — корпус капсулой и колесо
            return [rrect(-4, -6.75, 8, 13.5, 4), [[0, -3.75], [0, -1.25]]]
        case "battery":   // батарея (своё: раздел «Батарея») — корпус лёжа, пупочка справа, две полосы заряда
            return [rrect(-7, -3.5, 12, 7, 1.6), [[7, -1.2], [7, 1.2]], [[-4.5, -1], [-4.5, 1]], [[-2.2, -1], [-2.2, 1]]]
        case "folder":
            return [[[-7.2, -5.5], [-2.4, -5.5], [-0.4, -3.3], [7.2, -3.3], [7.2, 5.6], [-7.2, 5.6], [-7.2, -5.5]],
                    [[-7.2, -1.2], [7.2, -1.2]]]
        // значки кнопок окон НаомиОС (naomi-ui.c draw_glyph): нарисованы в 16 единицах от левого верхнего угла
        case "back": return [[[2, -4.5], [-2.5, 0], [2, 4.5]]]
        case "forward": return [[[-2, -4.5], [2.5, 0], [-2, 4.5]]]
        case "close": return [[[-4.5, -4.5], [4.5, 4.5]], [[4.5, -4.5], [-4.5, 4.5]]]
        case "minimize": return [[[-4.5, 0.5], [4.5, 0.5]]]
        case "maximize": return [rrect(-4.5, -4.5, 9, 9, 1)]
        case "restore": return [rrect(-4.5, -2.5, 7, 7, 1), [[-2.5, -2.5], [-2.5, -4.5], [4.5, -4.5], [4.5, 2.5], [2.5, 2.5]]]
        case "list": return [[[-1.5, -4], [5.5, -4]], [[-1.5, 0], [5.5, 0]], [[-1.5, 4], [5.5, 4]]]
        case "grid": return [rrect(-4.5, -4.5, 3.5, 3.5, 0.3), rrect(1, -4.5, 3.5, 3.5, 0.3), rrect(-4.5, 1, 3.5, 3.5, 0.3),
                             rrect(1, 1, 3.5, 3.5, 0.3)]
        case "wifi": {   // Wi-Fi: три дуги веером над точкой; шириной ≈ N_ICON (своего значка у НаомиОС нет)
            const cy = 4.6, a0 = -PI * 3 / 4, a1 = -PI / 4
            const out = [arc(0, cy, 3.4, a0, a1, 8), arc(0, cy, 6.6, a0, a1, 12), arc(0, cy, 9.8, a0, a1, 16)]
                .slice(g.arcFrom, g.arcTo)
            if (g.arcFrom === 0)
                out.push(arc(0, cy, 0.5, 0, 2 * PI, 8))
            return out
        }
        case "lan":      // интернет по кабелю (выбран пользователем, А3): узел сверху, провод — к двум узлам снизу
            return [rrect(-2.6, -6.25, 5.2, 4, 0.9), [[0, -2.25], [0, 0]], [[-4.6, 2.25], [-4.6, 0], [4.6, 0], [4.6, 2.25]],
                    rrect(-7, 2.25, 4.8, 4, 0.9), rrect(2.2, 2.25, 4.8, 4, 0.9)]
        case "lock": {   // замок закрытой сети (окно «Интернет»): дужка и корпус, высотой 11,5
            return [[[-2.6, -0.5]].concat(arc(0, -3, 2.6, PI, 2 * PI, 12), [[2.6, -0.5]]), rrect(-4, -0.5, 8, 6.25, 1.2)]
        }
        // путь трансляции (выбран пользователем 20.11.2026: «трансляция» 05.1, монитор — вариант 01): экран 14 × 9 со
        // скруглением 1,6 на ножке 3,5 с основанием 3,5; метка пути — в четверти круга радиусом 5 от точки m слева от
        // экрана на уровне основания (−10; 6,25). Всё ближе 8,25 к m срезано (5 + 0,75 + воздух 1,75 + 0,75): воздух
        // между меткой и монитором — 1,75 у обеих меток, у левой стороны, низа экрана и края основания. Без провода — две
        // дуги и точка (dots), кабель — вилка по диагонали: провод из m, корпус, штыри к экрану (концы — на радиусе 5)
        case "route-wifi":
            return route([arc(0, 0, 2.6, -PI / 2, 0, 10), arc(0, 0, 5, -PI / 2, 0, 16)])
        case "route-cable":
            return route([[[0, 0], [0.938, -0.938]], [[0, -1.875], [1.875, 0], [3.4375, -1.5625], [1.5625, -3.4375], [0, -1.875]],
                          [[3, -2], [4, -3]], [[2, -3], [3, -4]]])
        case "up": return [[[-3.5, 1.75], [0, -1.75], [3.5, 1.75]]]     // порядок колонки: по возрастанию
        case "down": return [[[-3.5, -1.75], [0, 1.75], [3.5, -1.75]]]  // по убыванию
        case "star": {    // пятиконечная звезда (звёздочка «в избранное» в строке списка)
            const pts = []
            for (let i = 0; i <= 10; ++i) {
                const r = i % 2 === 0 ? 6 : 6 * 0.45, a = -PI / 2 + i * PI / 5
                pts.push([r * Math.cos(a), r * Math.sin(a)])
            }
            return [pts]
        }
        case "search":    // лупа поиска в окне Bluetooth (bt_scan_button): круг и ручка, от верха круга до конца — 11,5
            return [arc(-1.25, -1.25, 4.5, 0, 2 * PI, 32), [[2.05, 2.05], [5.75, 5.75]]]
        case "plus": return [[[-6.25, 0], [6.25, 0]], [[0, -6.25], [0, 6.25]]]   // «+» новой вкладки (webbar_glyph)
        case "term": return [[[-6.5, -4.6], [-2, 0], [-6.5, 4.6]], [[0.6, 5], [6.6, 5]]]
        case "telegram":
            return [[[-7, -0.5], [7, -6], [4.5, 6.5], [-0.5, 2.5], [-3, 5], [-3, 1], [-7, -0.5]],
                    [[-3, 1], [7, -6], [-0.5, 2.5]]]
        case "deepseek": {   // минималистичный кит DeepSeek: купол головы, плавная спина, хвост и мягкий живот
            let body = [[-6.8, 0.8]]
            body = body.concat(bez([-6.8, 0.8], [-6.8, -4.6], [-2.2, -5.6], [1.4, -3.2], 12))
            body = body.concat(bez([1.4, -3.2], [3.4, -1.8], [4.4, -0.2], [5.4, -2.6], 8))
            body = body.concat(bez([5.4, -2.6], [6.2, -4.2], [7.2, -3.8], [6.6, -1.6], 6))
            body = body.concat(bez([6.6, -1.6], [6.2, -0.2], [6.9, 0.8], [7.1, 2.0], 6))
            body = body.concat(bez([7.1, 2.0], [6.0, 2.4], [4.8, 1.4], [4.0, 2.4], 6))
            body = body.concat(bez([4.0, 2.4], [2.4, 4.8], [-3.2, 5.2], [-5.8, 3.2], 10))
            body = body.concat(bez([-5.8, 3.2], [-6.6, 2.6], [-6.8, 1.8], [-6.8, 0.8], 4))
            return [body, [[-6.2, 2.1], [-2.6, 2.1]]]
        }
        case "claude": {
            const rays = []
            for (let i = 0; i < 8; ++i) {
                const a = -PI / 2 + i * PI / 4, r = i % 2 ? 4.6 : 6.5
                rays.push([[1.4 * Math.cos(a), 1.4 * Math.sin(a)], [r * Math.cos(a), r * Math.sin(a)]])
            }
            return rays
        }
        case "proc": return [[[-5.8, -3.6], [-1.8, 0], [-5.8, 3.6]], [[0.6, 4.2], [5.8, 4.2]]]
        case "panel":     // окошко 11 × 11 (draw_panel_mode_glyph), «голова» — заливкой (dots)
            return [[[-5.5, -5.5], [5.5, -5.5], [5.5, 5.5], [-5.5, 5.5], [-5.5, -5.5]]]
        case "split-left":
            return [[[-5.5, -4.5], [5.5, -4.5], [5.5, 4.5], [-5.5, 4.5], [-5.5, -4.5]], [[0, -4.5], [0, 4.5]]]
        case "split-right":
            return [[[-5.5, -4.5], [5.5, -4.5], [5.5, 4.5], [-5.5, 4.5], [-5.5, -4.5]], [[0, -4.5], [0, 4.5]]]
        case "camera":
            return [rrect(-6.5, -4, 13, 9, 1.5), [[-3, -4], [-1.8, -6], [1.8, -6], [3, -4]], arc(0, 0.5, 2.4, 0, 2 * PI, 20)]
        case "pin":       // булавка «Поверх всех окон» (Always-on-Top / PiP)
            return [[[-2.8, -5.2], [2.8, -5.2]], [[-1.8, -5.2], [-2.5, 0.2], [2.5, 0.2], [1.8, -5.2]], [[-4.2, 0.2], [4.2, 0.2]], [[0, 0.2], [0, 5.6]]]
        case "clipboard": // планшетка буфера обмена
            return [rrect(-5, -5.2, 10, 11.4, 1.4), rrect(-2.4, -6.6, 4.8, 2.4, 0.7), [[-2.6, -1.2], [2.6, -1.2]], [[-2.6, 1.8], [2.6, 1.8]], [[-2.6, 4.2], [1.0, 4.2]]]
        case "pen":       // карандаш 14 × 14 (draw_pen_glyph), от середины
            return [[[-5, 5], [-4.2, 2], [3, -5.2], [5.2, -3], [-2, 4.2], [-5, 5]], [[1.4, -3.6], [3.6, -1.4]]]
        case "trash":     // корзина 11,5 × 10,5 (своё, НаомиФона; как корзина «Файлов» НаомиОС, но с ручкой): крышка,
            // ручка, ведро сужается книзу, две черты внутри; на новой вкладке — те же точки (newtab.js, TRASH)
            return [[[-5.75, -3.25], [5.75, -3.25]], [[-2.25, -3.25], [-2.25, -5.25], [2.25, -5.25], [2.25, -3.25]],
                    [[-4.5, -3.25], [-3.75, 5.25], [3.75, 5.25], [4.5, -3.25]], [[-1.5, -0.75], [-1.5, 2.75]],
                    [[1.5, -0.75], [1.5, 2.75]]]
        case "dl-progress": return [[[0, -4.5], [0, 4]], [[-3.5, 0.5], [0, 4], [3.5, 0.5]]]   // загрузки 12 × 12
        case "dl-done": return [[[-4.5, 0.5], [-1.5, 3.5], [4.5, -3.5]]]
        case "dl-failed": return [[[-3.5, -3.5], [3.5, 3.5]], [[3.5, -3.5], [-3.5, 3.5]]]
        case "film": {    // кадр плёнки, по бокам — дорожки с перфорацией (APP_ICON_FILM)
            let out = [rrect(-7, -7, 14, 14, 1.6), [[-3.4, -7], [-3.4, 7]], [[3.4, -7], [3.4, 7]]]
            for (let i = -1; i <= 1; ++i)
                out.push([[-7, i * 3.5], [-3.4, i * 3.5]], [[3.4, i * 3.5], [7, i * 3.5]])
            return out
        }
        }
        return []
    }
    // заливкой: круги [cx, cy, r] и многоугольники [[x, y], …]
    readonly property var dots: {
        switch (kind) {
        case "speaker": return [[0, -3.4, 1.25]]
        case "route-wifi": return [[-8.375, 6.25, 1]]   // точка метки m со сдвигом значка (route)
        case "gamepad": return [[3.6, -0.4, 1.05], [5.9, -2.4, 1.05]]
        case "android": return [[-2.7, 1.4, 0.95], [2.7, 1.4, 0.95]]
        case "deepseek": return [[-3.8, -0.8, 0.9]]
        case "list": return [[-4.9, -4, 0.9], [-4.9, 0, 0.9], [-4.9, 4, 0.9]]
        case "panel": return [[[-5.5, -5.5], [5.5, -5.5], [5.5, -2], [-5.5, -2]]]
        case "split-left": return [[[-5.5, -4.5], [0, -4.5], [0, 4.5], [-5.5, 4.5]]]
        case "split-right": return [[[0, -4.5], [5.5, -4.5], [5.5, 4.5], [0, 4.5]]]
        case "reboot": {   // наконечник по касательной у конца дуги
            const a1 = 215 * Math.PI / 180, r = 6.25
            const ex = r * Math.cos(a1), ey = r * Math.sin(a1)
            const dx = -Math.sin(a1), dy = Math.cos(a1), nx = Math.cos(a1), ny = Math.sin(a1)
            return [[[ex + dx * 4.2, ey + dy * 4.2], [ex - dx * 0.4 + nx * 2.9, ey - dy * 0.4 + ny * 2.9],
                     [ex - dx * 0.4 - nx * 2.9, ey - dy * 0.4 - ny * 2.9]]]
        }
        }
        return []
    }

    Shape {
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: g.color
            strokeWidth: g.line
            fillColor: g.fill ? g.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathMultiline { paths: g.paths.map(p => g.pt(p)) }
        }
    }
    Repeater {
        model: g.dots
        delegate: Item {
            id: dot
            required property var modelData
            readonly property bool circle: typeof modelData[0] === "number"
            Rectangle {
                visible: dot.circle
                width: dot.circle ? 2 * dot.modelData[2] * g.unit : 0
                height: width
                radius: width / 2
                x: dot.circle ? dot.modelData[0] * g.unit - width / 2 : 0
                y: dot.circle ? dot.modelData[1] * g.unit - height / 2 : 0
                color: g.color
            }
            Shape {
                visible: !dot.circle
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: g.color
                    PathPolyline { path: dot.circle ? [] : g.pt(dot.modelData.concat([dot.modelData[0]])) }
                }
            }
        }
    }
    // «без звука»: прорезь цветом фона, поверх — черта. Только когда нужна: невидимая фигура всё равно создавалась в
    // каждом значке (окно «Файлов» собиралось дольше) и пересчитывалась при каждой смене цвета (~1,5 мс)
    Loader {
        active: g.slash
        sourceComponent: Shape {
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: g.cut; strokeWidth: g.line * 2.8; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                PathPolyline { path: g.pt([[-6.6, -6.2], [6.6, 6.2]]) }
            }
            ShapePath {
                strokeColor: g.color; strokeWidth: g.line; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                PathPolyline { path: g.pt([[-6.6, -6.2], [6.6, 6.2]]) }
            }
        }
    }
}
