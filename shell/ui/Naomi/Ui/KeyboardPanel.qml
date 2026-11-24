// Клавиатура в окне программы — одно целое с ней (docs/KEYBOARD.md, «В окне программы»). Окно программы не
// сжимается под клавиатуру: она выезжает внутри окна, а программа отступает снизу на visibleHeight — это одно
// и то же свойство одного кадра, поэтому поле ввода и лента едут с клавиатурой кадр в кадр, и при открытии,
// и при уходе, и под пальцем.
//   shown — насколько выехала (0…1); открывает и закрывает её модуль ввода (naomiKeyboard.visible): выезд
//   openMs, уход closeMs, кривая OutCubic.
//   Пальцем: dragTo(px) — клавиатура опущена на px от своего места (палец ведёт её вниз); release(vy) —
//   отпустили: вниз быстрее 250 точек/с или опущена больше чем на 40 % — уходит (спрятать: hide()), иначе
//   возвращается. После пальца — пружина, начатая со скоростью пальца (settle).
//   Полка над клавишами (как inputAccessoryView в iOS: панель разметки блокнота) — дети панели высотой
//   accessoryHeight: она часть клавиатуры, выезжает и уходит вместе с ней; visibleHeight её включает.
//   keyEdge и keyRadius — от края окна до крайней клавиши и скругление клавиши: по ним программа ровняет своё
//   (чат: облачко ввода и лента — по краям клавиш, с тем же скруглением).
import QtQuick
import Naomi.Ui

Item {
    id: panel
    anchors.left: parent.left
    anchors.right: parent.right
    height: kb.implicitHeight + accessoryHeight
    y: parent.height - visibleHeight

    property real accessoryHeight: 0
    property bool tablet: false
    readonly property real keyEdge: kb.edge
    readonly property real keyRadius: kb.keyRadius
    property real shown: 0
    readonly property real visibleHeight: height * Math.max(0, shown)
    readonly property bool open: naomiKeyboard !== null && naomiKeyboard.visible
    property bool dragging: false
    property real draggedAt: 0   // когда палец последний раз сдвинул клавиатуру (Date.now)
    signal dismissed()   // ушла пальцем — программе снять фокус с поля

    function animateTo(v) {
        settle.stop()
        anim.stop()
        const d = Math.abs(v - shown)
        if (d < 0.001) {
            shown = v
            return
        }
        anim.to = v
        anim.duration = (v > shown ? kb.openMs : kb.closeMs) * Math.max(0.3, d)
        anim.start()
    }
    // уход пальцем сам прячет модуль ввода — пружину, что уже ведёт клавиатуру туда же, не перебивать
    onOpenChanged: if (!dragging && !(settle.running && settle.target === (open ? 1 : 0)))
                       animateTo(open ? 1 : 0)
    Component.onCompleted: if (open) { if (tablet) animateTo(1); else shown = 1 }

    function dragTo(px) {
        dragging = true
        anim.stop()
        settle.stop()
        shown = Math.max(0, Math.min(1, 1 - px / panel.height))
        draggedAt = Date.now()
    }
    function release(vy) {
        if (!dragging)
            return
        dragging = false
        const close = vy > 250 || shown < 0.6
        // с последнего сдвига пальцем клавиатура уже «ехала» столько: шаг — в этом же кадре, без кадра на месте
        settle.go(close ? 0 : 1, -vy / panel.height, Math.min(0.05, (Date.now() - draggedAt) / 1000))
        if (close) {
            Qt.inputMethod.hide()
            dismissed()
        }
    }

    // касания по клавиатуре и полке — только им: мимо клавиш (глобус, поля, полка) палец не проходит к тому,
    // что под клавиатурой (поле заметки поставило бы туда курсор)
    MouseArea {
        anchors.fill: parent
        visible: panel.shown > 0
    }

    NumberAnimation {
        id: anim
        target: panel
        property: "shown"
        easing.type: Easing.OutCubic
    }

    // После пальца — затухающая пружина от скорости пальца, без рывка на отпускании. Как у iOS: уход клавиатуры
    // в Telegram, снятый с экрана iPhone, по кадрам ложится на ω = 26 1/с и затухание 0,8 (расхождение 4 точки на
    // 300). Уходит — с затуханием 0,8 (перелёт — за краем экрана, его не видно); возвращается — 1, без отскока.
    FrameAnimation {
        id: settle
        property real target: 0
        property real x0: 0     // shown − target в начале
        property real v0: 0     // скорость shown, 1/с
        property real t: 0
        property bool first: true
        property real zeta: 1
        readonly property real omega: 26

        function go(to, v, t0) {
            stop()
            target = to
            x0 = panel.shown - to
            v0 = v
            zeta = to === 0 ? 0.8 : 1
            t = t0
            first = true
            panel.shown = target + at(t)[0]
            start()
        }
        // отклонение от цели и скорость в момент t
        function at(t) {
            const w = omega, z = zeta
            if (z >= 1) {
                const e = Math.exp(-w * t), b = v0 + w * x0
                return [(x0 + b * t) * e, (b - w * (x0 + b * t)) * e]
            }
            const wd = w * Math.sqrt(1 - z * z), e = Math.exp(-z * w * t)
            const c = Math.cos(wd * t), s = Math.sin(wd * t), b = (v0 + z * w * x0) / wd
            return [e * (x0 * c + b * s), e * ((b * wd - z * w * x0) * c - (x0 * wd + z * w * b) * s)]
        }
        onTriggered: {
            t += first ? Math.min(Math.max(frameTime, 1 / 120), 1 / 45) : frameTime   // первый — от запуска
            first = false
            const [x, v] = at(t)
            if ((Math.abs(x) < 0.0005 && Math.abs(v) < 0.01) || t > 1.5) {
                panel.shown = target
                stop()
            } else {
                panel.shown = target + x
            }
        }
    }

    Keyboard {
        id: kb
        tablet: panel.tablet
        y: panel.accessoryHeight
        width: parent.width
        height: implicitHeight
        visible: panel.shown > 0
        onHideRequested: Qt.inputMethod.hide()
    }
}
