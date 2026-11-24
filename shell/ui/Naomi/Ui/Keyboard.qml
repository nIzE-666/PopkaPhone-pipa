// Клавиатура НаомиФона: русская ЙЦУКЕН, английская QWERTY, две страницы цифр и знаков. Одна и та же в оболочке
// (поверх, для её полей) и в каждой программе (в её окне — KeyboardPanel, docs/KEYBOARD.md). Печатает в поле в
// фокусе через naomiKeyboard (модуль ввода «naomi», shell/input/keyboard_context). Нажатия ловит одна область мультитача: можно печатать двумя пальцами внахлёст. Буква
// вводится при отпускании — та, что под пальцем в этот момент; «123» и «#+=» — сразу по касанию, а все страницы
// (русская, английская, две страницы знаков) созданы заранее — смена только показывает другую. Долгое нажатие: е → ё, ь → ъ, верхний ряд —
// цифры; «стереть» повторяется. Заглавная — сама в начале предложения; ⇧ дважды — капс. Поле просит иначе
// (подсказки ввода Qt): пароль, адрес — без заглавной сами по себе; «только латиница» — открыться на
// английской (свой выбор языка возвращается в обычном поле).
//
// Вид и места клавиш — 1:1 с клавиатурой iOS (снимок пользователя, 1125 пикселей в ширину = экран iPhone
// @3x): все размеры — пиксели снимка × s, s = ширина / 1125. Нижний ряд — «123», «Пробел», «Ввод»; под
// клавишами — глобус (язык) слева и «спрятать» справа (на месте микрофона). Цвета — свои (просьба
// пользователя): буквы и пробел — серые, служебные — темнее.
//
// Тачпад (как в iOS): пробел подержать 0,45 с или повести по нему пальцем вбок — клавиши гаснут, и палец водит
// курсор поля (naomiKeyboard.trackStart/trackMove/trackEnd), пробел при этом не печатается; отпустил — снова
// клавиатура.
//
// Опечатки исправляет naomiSpell (spell.h) — по точкам касаний, а не по буквам: пробел, знак препинания
// или ввод заменяют слово, если движок уверен; «стереть» сразу после исправления возвращает набранное и
// запоминает слово. Слово с заглавной посреди фразы (имя) не трогается. Полоски вариантов над клавишами
// нет — убрана по просьбе пользователя, исправление работает и без неё.
//
// Языки (функция Keyboard студии NaomiPhone): langs — "ru-en" (глобус переключает), "ru" или "en" (один, глобуса нет).
// Лежит в ~/.config/naomi/keyboard.ini — один файл на оболочку и программы (ставит оболочка: ShellView, функции
// проекта студии); программа читает его при запуске. Поле «только латиница» (пароль, адрес) — английская всегда.
import QtQuick
import QtCore
import Naomi.Ui

Item {
    id: root
    property bool tablet: false
    property alias langs: conf.langs    // ru-en | ru | en
    readonly property bool bothLangs: langs !== "ru" && langs !== "en"
    property string lang: "ru"          // ru | en
    property string userLang: "ru"      // язык, выбранный глобусом в обычном поле
    property bool latinForced: false    // поле ImhLatinOnly временно переключило на en
    Settings {
        id: conf
        location: StandardPaths.writableLocation(StandardPaths.GenericConfigLocation) + "/naomi/keyboard.ini"
        category: "keyboard"
        property string langs: "ru-en"
    }
    onLangsChanged: if (!bothLangs) {
        userLang = lang = langs   // один язык — сразу он
        latinForced = false
        syncToContext(lang)
    }
    property bool symbols: false        // цифры и знаки
    property bool more: false           // вторая страница знаков (#+=)
    property int shift: 0               // 0 — строчные, 1 — одна заглавная, 2 — капс
    signal hideRequested()

    // выезд и уход (просьба пользователя: чуть медленнее, чем 240 / 200 мс) — по ним же едет и окно программы
    readonly property int openMs: 320
    readonly property int closeMs: 280

    readonly property real s: tablet ? Math.min(width / 1125, 0.42) : width / 1125
    readonly property real edge: tablet ? 12 : 0
    readonly property color pressedColor: "#34332e"   // нажатая клавиша и буква над пальцем — светлее клавиш
    readonly property real keyGap: 14 * s      // между клавишами в ряду: на снимке 18, уже — клавиши шире (просьба пользователя)
    readonly property real unit: (width - 2 * edge + keyGap) / 11   // шаг клавиши: русский ряд — 11 букв
    readonly property real keyH: tablet ? Math.max(44, 130 * s) : 130 * s
    readonly property real keyRadius: 15 * s   // скругление клавиши (по нему же — облачко ввода чата)
    readonly property real gap: tablet ? 8 : 33 * s
    readonly property real topPad: 22 * s
    readonly property real keysBottom: topPad + 4 * keyH + 3 * gap
    // Палец ложится ниже, чем человек целится. Замер пользователя (11 ноября 2026, фраза трижды, 72 буквы,
    // tools/kbtaps): касания ниже центра клавиши на 64 пикселя экрана в верхнем ряду, 38 — в среднем, 11 — в
    // нижнем; все 9 промахов — клавиша прямо под нужной. Точка касания поднимается на 30 пикселей экрана (31 —
    // снимка): промахов 1 из 72 (docs/KEYBOARD.md, «Попадание»). Клавиши на экране — где были
    readonly property real touchLift: tablet ? 0 : 31 * s
    readonly property real iconsY: keysBottom + (tablet ? 26 : 119 * s)
    implicitHeight: keysBottom + (tablet ? 52 : 239 * s)

    readonly property var letters: ({
        ru: ["йцукенгшщзх", "фывапролджэ", "ячсмитьбю"],
        en: ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
    })
    readonly property var signs: [
        ["1234567890", "-/:;()₽&@\""],     // «₽» у английской — «$» (label)
        ["[]{}#%^*+=", "_\\|~<>€£¥•"]
    ]
    readonly property var alts: ({ "е": "ё", "ь": "ъ" })

    // Раскладки всех страниц (русская, английская, две страницы знаков) — один раз, меняются только с шириной:
    // смена страницы лишь показывает другую, клавиши заново не создаются. Замер (11 ноября 2026): пересоздание
    // клавиш при «123» — 33–75 мс, вместе с отпусканием пальца — ≈ 180 мс от касания до кадра
    readonly property string page: symbols ? (more ? "sym1" : "sym0") : lang
    function layoutOf(page) {
        const chars = (str, w) => str.split("").map(c => ({ kind: "char", text: c, w: w }))
        const rows = []
        if (page === "sym0" || page === "sym1") {
            const src = signs[page === "sym1" ? 1 : 0]
            rows.push(chars(src[0], 1.1), chars(src[1], 1.1))
            rows.push([{ kind: "more", w: 1.4 }, { kind: "gap", w: 0.25 }].concat(chars(".,?!'", 1.54),
                      [{ kind: "gap", w: 0.25 }, { kind: "bksp", w: 1.4 }]))
        } else if (page === "ru") {
            const src = letters.ru
            rows.push(chars(src[0], 1), chars(src[1], 1))
            rows.push([{ kind: "shift", w: 1 }].concat(chars(src[2], 1), [{ kind: "bksp", w: 1 }]))
        } else {
            const src = letters.en
            rows.push(chars(src[0], 1.1), chars(src[1], 1.1))
            rows.push([{ kind: "shift", w: 1.45 }, { kind: "gap", w: 0.2 }].concat(chars(src[2], 1.1),
                      [{ kind: "gap", w: 0.2 }, { kind: "bksp", w: 1.45 }]))
        }
        rows.push([{ kind: "mode", w: 2.75 }, { kind: "space", w: 5.5 }, { kind: "enter", w: 2.75 }])
        const out = []
        rows.forEach((row, i) => {
            let x = (width - row.reduce((sum, k) => sum + k.w, 0) * unit) / 2
            row.forEach((k, j) => {
                out.push(Object.assign({ page: page, row: i, col: j, x: x, y: topPad + i * (keyH + gap),
                                         kw: k.w * unit }, k))
                x += k.w * unit
            })
        })
        return out
    }
    readonly property var pages: ["ru", "en", "sym0", "sym1"]
    readonly property var layouts: ({ ru: layoutOf("ru"), en: layoutOf("en"), sym0: layoutOf("sym0"),
                                      sym1: layoutOf("sym1") })
    readonly property var keys: layouts[page]   // клавиши страницы на экране

    function label(k) {
        if (k.kind !== "char")
            return ""
        if (k.text === "₽" && lang !== "ru")
            return "$"
        return shift > 0 && (k.page === "ru" || k.page === "en") ? k.text.toUpperCase() : k.text
    }
    function altFor(k) {
        if (k.kind !== "char" || k.page === "sym0" || k.page === "sym1")
            return ""
        const a = alts[k.text] ?? (k.row === 0 && k.col < 10 ? "1234567890"[k.col] : "")
        return shift > 0 ? a.toUpperCase() : a
    }

    // какая клавиша под точкой: промежутки делятся между соседями, края ряда — крайним клавишам
    function keyAt(x, y) {
        if (y > keysBottom + gap / 2)
            return -1
        const row = Math.max(0, Math.min(3, Math.floor((y - topPad + gap / 2) / (keyH + gap))))
        let best = -1
        for (let i = 0; i < keys.length; ++i) {
            const k = keys[i]
            if (k.row !== row)
                continue
            if (best < 0 || x >= k.x)
                best = i
        }
        if (best >= 0 && keys[best].kind === "gap")   // пустое место между клавишами — ближайшей
            best = x < keys[best].x + keys[best].kw / 2 ? best - 1 : best + 1
        return best
    }

    // ввод
    function type(text) {
        naomiKeyboard.commit(text)
        if (shift === 1)
            shift = 0
        autoShift()
    }
    function noAutoCaps() {
        return (naomiKeyboard.hints() & (Qt.ImhNoAutoUppercase | Qt.ImhHiddenText | Qt.ImhSensitiveData
                                         | Qt.ImhUrlCharactersOnly | Qt.ImhEmailCharactersOnly)) !== 0
    }
    function autoShift() {
        if (shift === 2 || symbols)
            return
        shift = !noAutoCaps() && /(^|[.!?…]\s+|\n\s*)$/.test(naomiKeyboard.textBeforeCursor()) ? 1 : 0
        shiftAuto = shift === 1
    }
    // пробел: в знаках — обратно к буквам; два пробела подряд после слова — «. » (как в iOS)
    property real spaceAt: 0
    function space() {
        const now = Date.now()
        if (!symbols && !noAutoCaps() && now - spaceAt < 600
                && /[^\s.,!?…:;]\s$/.test(naomiKeyboard.textBeforeCursor())) {
            naomiKeyboard.key(Qt.Key_Backspace)
            spaceAt = 0
            type(". ")
            return
        }
        spaceAt = now
        if (symbols) {
            symbols = false
            more = false
        }
        if (finishWord(" ")) {
            if (shift === 1)
                shift = 0
            autoShift()
        } else {
            type(" ")
        }
    }
    property real shiftTapAt: 0
    function activate(k, x, y) {
        if (k.kind !== "space")
            spaceAt = 0
        undo = null
        switch (k.kind) {
        case "char": typeChar(label(k), x, y, symbols); break
        case "space": space(); break
        case "enter":
            finishWord("")
            naomiKeyboard.key(Qt.Key_Return)
            if (shift !== 2 && !symbols && !noAutoCaps())
                shift = 1
            break
        case "mode": symbols = !symbols; more = false; shift = 0; if (!symbols) autoShift(); break
        case "more": more = !more; break
        case "shift": {
            const now = Date.now()
            shiftAuto = false
            shift = shift === 2 ? 0 : (shift === 1 && now - shiftTapAt < 350) ? 2 : (shift === 1 ? 0 : 1)
            shiftTapAt = now
            break
        }
        }
    }
    function syncFromContext() {
        if (!tablet || !bothLangs || !naomiKeyboard)
            return
        const ctxLang = naomiKeyboard.layout.toLowerCase()
        if ((ctxLang !== "ru" && ctxLang !== "en") || ctxLang === lang)
            return
        resetWord()
        lang = ctxLang
        latinForced = false
        if (!(naomiKeyboard.hints() & Qt.ImhLatinOnly))
            userLang = ctxLang
    }
    function syncToContext(code) {
        if (tablet && naomiKeyboard && (code === "ru" || code === "en"))
            naomiKeyboard.setLayout(code.toUpperCase())
    }
    function switchLang() {
        if (!bothLangs)
            return
        resetWord()
        lang = lang === "ru" ? "en" : "ru"
        latinForced = false
        if (!(naomiKeyboard.hints() & Qt.ImhLatinOnly))
            userLang = lang
        syncToContext(lang)
    }

    // ---------- исправление опечаток ----------
    property var word: []          // касания текущего слова: {x, y, ch, exact}
    property string typed: ""      // как набрано
    property var result: null      // разбор naomiSpell.decode после последней буквы: что подставит пробел
    property var undo: null        // {original, replaced}: только что исправили — «стереть» вернёт набранное
    property bool shiftAuto: false // заглавную поставила сама клавиатура (начало предложения)
    property bool capAuto: false   // первая буква слова — такой заглавной
    function canPredict() {
        return visible && (naomiKeyboard.hints() & (Qt.ImhHiddenText | Qt.ImhSensitiveData
            | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly | Qt.ImhEmailCharactersOnly | Qt.ImhDigitsOnly
            | Qt.ImhDialableCharactersOnly)) === 0
    }
    // центры букв и разброс касаний — движку (σ: 0,45 шага клавиш и 0,38 шага рядов — подобрано пробой)
    function spellKeys() {
        if (!symbols)
            naomiSpell.setKeys(keys.filter(k => k.kind === "char").map(k => ({ ch: k.text, x: k.x + k.kw / 2,
                                                                               y: k.y + keyH / 2 })),
                               0.45 * unit, 0.38 * (keyH + gap))
    }
    onKeysChanged: spellKeys()
    Component.onCompleted: {
        spellKeys()
        if (!bothLangs) {
            userLang = lang = langs   // один язык — сразу он
            syncToContext(lang)
        } else {
            syncFromContext()
        }
    }

    function resetWord() {
        word = []
        typed = ""
        result = null
    }
    function wordIntact() {   // курсор там, где кончается набранное слово (не уводили, программа не меняла)
        return typed.length > 0 && naomiKeyboard.textBeforeCursor().endsWith(typed)
    }
    // буква или знак: буквы (и дефис, апостроф внутри слова) копят слово, знак препинания его заканчивает
    function typeChar(t, x, y, exact) {
        if (/[a-zа-яё]/i.test(t) || (typed.length && /^[-']$/.test(t))) {
            if (!typed.length)
                capAuto = shift === 1 && shiftAuto
            type(t)
            if (!canPredict())
                return
            word = word.concat([{ x: x, y: y, ch: t, exact: exact }])
            typed += t
            result = naomiSpell.decode(lang, word)
        } else if (/^[.,!?:;…]$/.test(t) && finishWord(t)) {
            if (shift === 1)
                shift = 0
            autoShift()
        } else {
            resetWord()
            type(t)
        }
    }
    // конец слова: исправить, если уверены (tail — пробел или знак — дописать тем же событием)
    function finishWord(tail) {
        if (!typed.length || !canPredict()) {
            resetWord()
            return false
        }
        const r = result, intact = wordIntact()
        const name = typed[0] !== typed[0].toLowerCase() && !capAuto
        let done = false
        if (intact && r && r.correct && !name && r.best !== typed) {
            naomiKeyboard.replaceBefore(typed.length, r.best + tail)
            undo = { original: typed, replaced: r.best + tail }
            done = true
        } else if (intact && r && !r.known) {
            naomiSpell.learn(lang, typed, 1)
        }
        resetWord()
        return done
    }
    function backspace() {
        if (undo && naomiKeyboard.textBeforeCursor().endsWith(undo.replaced)) {   // вернуть набранное
            naomiKeyboard.replaceBefore(undo.replaced.length, undo.original)
            naomiSpell.learn(lang, undo.original, 2)
            undo = null
            return false
        }
        undo = null
        naomiKeyboard.key(Qt.Key_Backspace)
        if (typed.length) {
            typed = typed.slice(0, -1)
            word = word.slice(0, -1)
            result = typed.length ? naomiSpell.decode(lang, word) : null
        }
        return true
    }

    // пальцы: pointId → { key, used, x, y, x0 }; used — клавиша уже сработала (долгое нажатие, «стереть», тачпад)
    property var held: ({})
    property int padId: -1                 // палец, что ведёт тачпад; −1 — тачпада нет
    readonly property bool padActive: padId >= 0
    function startPad(id) {
        const h = held[id]
        if (!h || padActive || !naomiKeyboard.trackStart())
            return
        if (typeof naomiHaptics !== "undefined" && naomiHaptics)
            naomiHaptics.tap(30)   // пробел стал тачпадом — отклик подлиннее
        padId = id
        h.used = true
        hold.stop()
        previewKey = -1
        resetWord()
        undo = null
        spaceAt = 0
        updateDown()
    }
    Timer {
        id: padHold
        interval: 450
        property int pointId: -1
        onTriggered: {
            const h = root.held[pointId]
            if (h && !h.used && root.keys[h.key].kind === "space")
                root.startPad(pointId)
        }
    }
    property var downKeys: []
    property int previewKey: -1
    function updateDown() {
        downKeys = Object.keys(held).map(id => held[id].key)
    }
    function press(id, x, y) {
        const i = keyAt(x, y)
        if (i < 0)
            return
        // отклик вибромотором (haptics.h); его нет вне оболочки и на телефоне без мотора
        if (typeof naomiHaptics !== "undefined" && naomiHaptics)
            naomiHaptics.tap(12)
        const kind = keys[i].kind
        if (kind === "mode" || kind === "more") {   // страница — сразу по касанию (как в iOS), не по отпусканию
            activate(keys[i], x, y)
            held[id] = { key: keys.findIndex(k => k.kind === kind), used: true, x: x, y: y }
            updateDown()
            return
        }
        held[id] = { key: i, used: false, x: x, y: y, x0: x }
        startHold(id, i)
        if (kind === "space") {
            padHold.pointId = id
            padHold.restart()
        }
        updateDown()
    }
    function move(id, x, y) {
        const h = held[id]
        if (padActive) {   // тачпад: палец водит курсор, остальные пальцы молчат
            if (id === padId && h) {
                naomiKeyboard.trackMove(x - h.x, y - h.y)
                h.x = x
                h.y = y
            }
            return
        }
        if (!h || h.used)
            return
        if (keys[h.key].kind === "space" && Math.abs(x - h.x0) > 14) {   // повёл по пробелу вбок — тачпад сразу
            startPad(id)
            if (padActive) {
                naomiKeyboard.trackMove(x - h.x, y - h.y)
                h.x = x
                h.y = y
                return
            }
        }
        h.x = x
        h.y = y
        const i = keyAt(x, y)
        if (i >= 0 && i !== h.key) {
            h.key = i
            startHold(id, i)
            updateDown()
        }
    }
    function release(id, commit) {
        const h = held[id]
        if (!h)
            return
        delete held[id]
        if (hold.pointId === id)
            hold.stop()
        if (padHold.pointId === id)
            padHold.stop()
        if (id === padId) {
            padId = -1
            naomiKeyboard.trackEnd()
        }
        if (commit && !h.used)
            activate(keys[h.key], h.x, h.y)
        updateDown()
    }
    function startHold(id, i) {
        const k = keys[i]
        hold.stop()
        hold.pointId = id
        previewKey = k.kind === "char" ? i : -1
        if (k.kind === "bksp") {
            held[id].used = true
            spaceAt = 0
            if (backspace()) {   // вернула исправленное — не повторять
                hold.interval = 420
                hold.start()
            }
            reshift.restart()
        } else if (altFor(k) !== "") {
            hold.interval = 420
            hold.start()
        }
    }
    onDownKeysChanged: if (downKeys.length === 0) previewKey = -1
    onVisibleChanged: {
        if (padActive) {
            padId = -1
            naomiKeyboard.trackEnd()
        }
        held = {}
        downKeys = []
        resetWord()
        undo = null
        if (visible) {
            symbols = false
            more = false
            if (tablet && bothLangs && naomiKeyboard) {
                const ctxLang = naomiKeyboard.layout.toLowerCase()
                if (ctxLang === "ru" || ctxLang === "en")
                    userLang = ctxLang
            }
            const latinOnly = (naomiKeyboard.hints() & Qt.ImhLatinOnly) !== 0
            latinForced = latinOnly && userLang !== "en"
            lang = latinOnly ? "en" : userLang
            syncToContext(lang)
            if (shift !== 2)
                autoShift()
        } else if (latinForced) {
            latinForced = false
            syncToContext(userLang)
        }
    }
    Connections {
        target: naomiKeyboard
        function onLayoutChanged() { root.syncFromContext() }
        function onFocusObjectChanged() {
            if (!root.visible)
                return
            root.resetWord()
            root.undo = null
            if (root.tablet && root.bothLangs) {
                const latinOnly = (naomiKeyboard.hints() & Qt.ImhLatinOnly) !== 0
                if (latinOnly && root.lang !== "en") {
                    root.latinForced = true
                    root.lang = "en"
                    root.syncToContext("en")
                } else if (!latinOnly && root.latinForced) {
                    root.latinForced = false
                    root.lang = root.userLang
                    root.syncToContext(root.userLang)
                }
            }
            if (root.shift !== 2)
                root.autoShift()
        }
    }

    Timer {
        id: hold
        property int pointId: -1
        repeat: true
        onTriggered: {
            const h = root.held[pointId]
            if (!h) {
                stop()
                return
            }
            const k = root.keys[h.key]
            if (k.kind === "bksp") {
                root.backspace()
                interval = 60
                reshift.restart()
            } else {
                root.spaceAt = 0
                root.undo = null
                root.typeChar(root.altFor(k), h.x, h.y, true)
                h.used = true
                root.previewKey = -1
                stop()
            }
        }
    }
    // после «стереть» текст до курсора у программы обновляется не сразу — заглавную решаем чуть позже
    Timer { id: reshift; interval: 120; onTriggered: root.autoShift() }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
        Rectangle { width: parent.width; height: 1; color: Theme.surfaceHigh }
        Rectangle { visible: root.tablet; width: 1; height: parent.height; color: Theme.surfaceHigh }
        Rectangle { visible: root.tablet; x: parent.width - 1; width: 1; height: parent.height; color: Theme.surfaceHigh }
    }

    Repeater {   // все страницы созданы заранее, видна одна (page)
        model: root.pages
        Item {
            id: pageItem
            required property string modelData
            readonly property bool active: root.page === modelData
            anchors.fill: parent
            visible: active
            Repeater {
                model: root.layouts[pageItem.modelData]
                delegate: Rectangle {
                    id: key
                    required property var modelData
                    required property int index
                    readonly property string kind: modelData.kind
                    readonly property bool down: pageItem.active && root.downKeys.indexOf(index) >= 0
                    readonly property bool fn: kind !== "char" && kind !== "space"
                    visible: kind !== "gap"
                    x: modelData.x + root.keyGap / 2
                    y: modelData.y
                    width: modelData.kw - root.keyGap
                    height: root.keyH
                    radius: root.keyRadius
                    // без янтаря и рамок (просьба пользователя): нажатая — светлее, включённый ⇧ — залитая стрелка;
                    // тачпад — все клавиши одного цвета и без надписей (как в iOS)
                    color: root.padActive ? Theme.surface : down ? root.pressedColor : fn ? Theme.surface : Theme.surfaceHigh
                    Behavior on color { enabled: root.padActive; ColorAnimation { duration: 150 } }

                    Text {   // буквы и знаки
                        opacity: root.padActive ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        anchors.centerIn: parent
                        visible: key.kind === "char"
                        text: root.label(key.modelData)
                        font.family: Theme.sans
                        font.pixelSize: 24
                        color: Theme.text
                    }
                    Text {   // ⇧ и ⌫ — значки Material Symbols, как SF Symbols в iOS
                        opacity: root.padActive ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        anchors.centerIn: parent
                        visible: key.kind === "shift" || key.kind === "bksp"
                        text: key.kind === "bksp" ? "backspace" : root.shift === 2 ? "shift_lock" : "shift"
                        font.family: "Material Symbols Rounded"
                        font.pixelSize: 24
                        font.variableAxes: ({ "FILL": key.kind === "shift" && root.shift > 0 ? 1 : 0, "wght": 300 })
                        renderType: Text.NativeRendering   // у залитого значка контуры накладываются — по полю расстояний контур двоился
                        color: Theme.text
                    }
                    Text {   // «123», «#+=», «Пробел», «Ввод»
                        opacity: root.padActive ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        anchors.centerIn: parent
                        visible: text !== ""
                        text: ({ mode: key.modelData.page.startsWith("sym") ? (root.lang === "ru" ? "АБВ" : "ABC") : "123",
                                 more: key.modelData.page === "sym1" ? "123" : "#+=",
                                 space: root.lang === "ru" ? "Пробел" : "space",
                                 enter: root.lang === "ru" ? "Ввод" : "return" })[key.kind] ?? ""
                        font.family: Theme.sans
                        font.pixelSize: key.kind === "more" ? 15 : 17
                        color: Theme.text
                    }
                }
            }
        }
    }

    // увеличенная буква над пальцем; над ней — что даст долгое нажатие
    Rectangle {
        readonly property var k: root.previewKey >= 0 ? root.keys[root.previewKey] : null
        visible: k !== null
        width: k ? Math.max(k.kw + 10, 46) : 0
        height: 66
        x: k ? Math.max(0, Math.min(root.width - width, k.x + k.kw / 2 - width / 2)) : 0
        y: k ? k.y - height + 8 : 0
        radius: 10
        color: root.pressedColor
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12
            text: parent.k ? root.label(parent.k) : ""
            font.family: Theme.sans
            font.pixelSize: 30
            color: Theme.text
        }
        Caps {
            anchors.right: parent.right
            anchors.rightMargin: 5
            y: 4
            text: parent.k ? root.altFor(parent.k) : ""
            font.pixelSize: 10
        }
    }

    MultiPointTouchArea {
        width: parent.width
        height: root.keysBottom + root.gap / 2
        mouseEnabled: true
        onPressed: (points) => { for (const p of points) root.press(p.pointId, p.x, p.y - root.touchLift) }
        onUpdated: (points) => { for (const p of points) root.move(p.pointId, p.x, p.y - root.touchLift) }
        onReleased: (points) => { for (const p of points) root.release(p.pointId, true) }
        onCanceled: (points) => { for (const p of points) root.release(p.pointId, false) }
    }

    // под клавишами, как в iOS: глобус — язык (слева), «спрятать» — справа, на месте микрофона
    Repeater {
        model: [{ icon: "language", cx: 125, act: "lang" }, { icon: "keyboard_hide", cx: 1000, act: "hide" }]
        Text {
            required property var modelData
            visible: modelData.act !== "lang" || root.bothLangs   // один язык — глобуса нет
            x: modelData.cx * root.width / 1125 - width / 2
            y: root.iconsY - height / 2
            font.family: "Material Symbols Rounded"
            font.pixelSize: 30
            font.variableAxes: ({ "wght": 300 })
            color: tap.pressed ? Theme.muted : Theme.text
            text: modelData.icon
            TapHandler {
                id: tap
                margin: 14
                onTapped: modelData.act === "lang" ? root.switchLang() : root.hideRequested()
            }
        }
    }
}
