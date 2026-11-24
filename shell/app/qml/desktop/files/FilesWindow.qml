// «Файлы» рабочего стола — перенос «Файлов» НаомиОС (src/files*.c там) 1 в 1, на файловой системе телефона
// (docs/FILES.md). Окно: полоса (назад, вперёд, путь; плитка / список), слева места — «01 МЕСТА»: Дом,
// Избранное, Корзина (с числом), «02 СВОИ» — закреплённые папки (закладки GTK); справа — папка плиткой или
// списком (ИМЯ, ТИП, СОЗДАН / УДАЛЁН, РАЗМЕР, звёздочка), внизу — строка: сколько выделено, ход операций,
// ОТМЕНИТЬ / ОСТАНОВИТЬ. Меню по правому щелчку, имя — на месте (F2), «удалить навсегда» — с вопросом,
// перетаскивание — в папки, места и другие окна. Клавиши — как в НаомиОС (keymap — docs/FILES.md).
// Данные — naomiFiles (FilesService) и FilesModel (shell/files).
import QtQuick
import Naomi.Files
import ".."

DeskWindow {
    id: fw
    minimumSize: Qt.size(640, 420)
    property Item desk                       // стол: перетаскивание между окнами (Desktop.qml)
    property string start: naomiFiles.home
    signal openWindow(string location)       // «Открыть в новом окне», Ctrl+N

    // Окно из запаса (Desktop.qml, openFiles): собрано заранее, пока ждало — вид, порядок и скрытые могли смениться
    // в других окнах; новое окно берёт их свежими, как созданное только что
    function freshen(location) {
        mode = naomiFiles.setting("view/mode", "grid")
        files.showHidden = flag(naomiFiles.setting("view/hidden", false))
        files.sortKey = naomiFiles.setting("view/sort", "name")
        files.descending = naomiFiles.setting("view/order", "asc") === "desc"
        start = location
    }

    readonly property bool realDir: !files.trash && !files.starredPlace
    readonly property bool listMode: mode === "list"
    property string mode: naomiFiles.setting("view/mode", "grid")
    readonly property Item view: listMode ? listView : gridView

    function flag(v) { return v === true || v === "true" }
    function plural(n, one, few, many) {
        const m10 = n % 10, m100 = n % 100
        return m10 === 1 && m100 !== 11 ? one : m10 >= 2 && m10 <= 4 && (m100 < 10 || m100 >= 20) ? few : many
    }
    function fileUrl(path) { return "file://" + encodeURIComponent(path).replace(/%2F/g, "/") }
    function dimA(a) { return Qt.rgba(DeskStyle.dim.r, DeskStyle.dim.g, DeskStyle.dim.b, a) }
    function badA(a) { return Qt.rgba(DeskStyle.bad.r, DeskStyle.bad.g, DeskStyle.bad.b, a) }

    FilesModel {
        id: files
        location: fw.start
        showHidden: fw.flag(naomiFiles.setting("view/hidden", false))
        sortKey: naomiFiles.setting("view/sort", "name")
        descending: naomiFiles.setting("view/order", "asc") === "desc"
        onRowsRemoved: fw.pruneSelection()
        onRowsInserted: fw.checkPending()
        onModelReset: { fw.pruneSelection(); fw.checkPending() }
    }

    // ---------- детали окна ----------
    component SidePlate: Item {
        id: sp
        property string num
        property string title
        x: 14
        width: 232 - 28
        height: 14
        Text {
            id: spNum
            anchors.verticalCenter: parent.verticalCenter
            text: sp.num
            font.family: DeskStyle.mono
            font.pixelSize: 11
            color: DeskStyle.dim
        }
        Text {
            id: spTitle
            x: spNum.implicitWidth + 8
            anchors.verticalCenter: parent.verticalCenter
            text: sp.title
            font.family: DeskStyle.mono
            font.pixelSize: 11
            font.letterSpacing: 2.4
            color: DeskStyle.amber
        }
        Rectangle {
            x: spTitle.x + spTitle.implicitWidth + 8
            width: Math.max(0, sp.width - x)
            height: 1
            anchors.verticalCenter: parent.verticalCenter
            color: DeskStyle.amberA(DeskStyle.line)
        }
    }
    component PlaceRow: Item {
        id: pr
        property string label
        property string place
        property string count
        property bool pinned: false
        readonly property bool active: files.location === place
        readonly property bool target: fw.dropKey === "place:" + place
        x: 12
        width: 232 - 24
        height: 31 + 2
        Rectangle {
            y: 1
            width: parent.width; height: parent.height - 2
            color: pr.target ? DeskStyle.amberA(0.24) : pr.active ? DeskStyle.amberA(0.10)
                 : prMouse.containsMouse ? DeskStyle.amberA(0.08) : "transparent"
        }
        Rectangle {
            y: 1
            width: 3; height: parent.height - 2
            color: pr.active ? DeskStyle.amber : fw.dimA(0.45)
        }
        Text {
            x: 15
            width: parent.width - x - 12 - (cnt.visible ? cnt.implicitWidth + 8 : 0)
            anchors.verticalCenter: parent.verticalCenter
            text: pr.label
            elide: Text.ElideRight
            font.family: DeskStyle.sans
            font.pixelSize: 14
            color: pr.active || prMouse.containsMouse ? DeskStyle.text : DeskStyle.muted
        }
        Text {
            id: cnt
            visible: pr.count !== ""
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: pr.count
            font.family: DeskStyle.mono
            font.pixelSize: 11
            color: DeskStyle.dim
        }
        MouseArea {
            id: prMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: (m) => {
                keys.forceActiveFocus()
                if (m.button === Qt.RightButton) {
                    const q = mapToItem(menu, m.x, m.y)
                    fw.placeMenu(q.x, q.y, pr.place, pr.pinned)
                } else {
                    fw.go(pr.place)
                }
            }
        }
    }
    component StatusText: Text {
        anchors.verticalCenter: parent.verticalCenter
        font.family: DeskStyle.mono
        font.pixelSize: 11
        font.letterSpacing: 1.5
    }
    component RenameField: Rectangle {   // имя на месте: поверхность, янтарная рамка .55, выделено имя без типа
        id: rf
        property bool tileStyle: false
        property string name
        property bool isDir
        property alias text: input.text
        height: input.implicitHeight + (tileStyle ? 4 : 6)
        color: DeskStyle.surface
        border.width: 1
        border.color: DeskStyle.amberA(0.55)
        radius: 3
        TextInput {
            id: input
            x: rf.tileStyle ? 4 : 6
            width: parent.width - 2 * x
            anchors.verticalCenter: parent.verticalCenter
            text: rf.name
            horizontalAlignment: rf.tileStyle ? TextInput.AlignHCenter : TextInput.AlignLeft
            font.family: DeskStyle.sans
            font.pixelSize: rf.tileStyle ? 13 : 14
            color: DeskStyle.text
            selectionColor: DeskStyle.amberA(0.35)
            selectedTextColor: DeskStyle.text
            selectByMouse: true
            clip: true
            cursorDelegate: Rectangle { width: 1; color: DeskStyle.amber }
            Keys.onReturnPressed: fw.endRename(true)
            Keys.onEnterPressed: fw.endRename(true)
            Keys.onEscapePressed: fw.endRename(false)
            onActiveFocusChanged: if (!activeFocus && fw.renameField === rf) Qt.callLater(() => fw.endRename(true))
            Component.onCompleted: {
                fw.renameField = rf
                forceActiveFocus()
                const dot = rf.isDir ? -1 : text.lastIndexOf(".")
                select(0, dot > 0 ? dot : text.length)
            }
        }
    }

    // ---------- место и история ----------
    property var backStack: []
    property var fwdStack: []
    function go(loc, how) {   // how: пусто — в историю; "back", "forward" — по ней
        if (!loc)
            return
        endRename(true)
        pathEdit.active = false
        if (loc === files.location) {
            files.reload()
            return
        }
        if (how === "back")
            fwdStack = fwdStack.concat([files.location])
        else if (how === "forward")
            backStack = backStack.concat([files.location])
        else {
            backStack = backStack.concat([files.location])
            fwdStack = []
        }
        files.location = loc
        clearSelection()
        gridView.positionViewAtBeginning()
        listView.positionViewAtBeginning()
    }
    function goBack() {
        if (!backStack.length)
            return
        const l = backStack[backStack.length - 1]
        backStack = backStack.slice(0, -1)
        go(l, "back")
    }
    function goForward() {
        if (!fwdStack.length)
            return
        const l = fwdStack[fwdStack.length - 1]
        fwdStack = fwdStack.slice(0, -1)
        go(l, "forward")
    }
    function goUp() {
        const p = naomiFiles.parentOf(files.location)
        if (!p)
            return
        const from = files.location
        go(p)
        selectPath(from)   // наверх — выделена папка, из которой пришли
    }
    function setMode(m) {
        mode = m
        naomiFiles.setSetting("view/mode", m)
        if (cursor >= 0)
            reveal(cursor)
    }
    function setSort(key) {
        if (files.sortKey === key)
            files.descending = !files.descending
        else {
            files.sortKey = key
            files.descending = false
        }
        naomiFiles.setSetting("view/sort", files.sortKey)
        naomiFiles.setSetting("view/order", files.descending ? "desc" : "asc")
    }
    function toggleHidden() {
        files.showHidden = !files.showHidden
        naomiFiles.setSetting("view/hidden", files.showHidden)
    }

    // ---------- выделение: пути (переживает пересортировку и обновление папки) ----------
    property var sel: ({})
    property int selCount: 0
    property int cursor: -1          // строка «в фокусе» (стрелки)
    property int anchorRow: -1       // откуда тянется Shift
    property bool keyNav: false      // рамка фокуса — только когда ходят клавишами
    function setSelection(paths) {
        const m = {}
        for (const p of paths)
            m[p] = true
        sel = m
        selCount = Object.keys(m).length
    }
    function selectedPaths() {
        const out = []
        for (let i = 0; i < files.count; ++i)
            if (sel[files.pathAt(i)])
                out.push(files.pathAt(i))
        return out
    }
    function firstSelected() {
        for (let i = 0; i < files.count; ++i)
            if (sel[files.pathAt(i)])
                return i
        return -1
    }
    function selectOnly(row) {
        setSelection(row >= 0 ? [files.pathAt(row)] : [])
        cursor = row
        anchorRow = row
        if (row >= 0)
            reveal(row)
    }
    function selectPath(path) {
        const i = files.indexOf(path)
        if (i >= 0)
            selectOnly(i)
        return i >= 0
    }
    function toggleRow(row) {
        const p = files.pathAt(row), m = Object.assign({}, sel)
        if (m[p])
            delete m[p]
        else
            m[p] = true
        sel = m
        selCount = Object.keys(m).length
        cursor = row
        anchorRow = row
    }
    function selectRange(a, b) {
        const m = {}
        for (let i = Math.min(a, b); i <= Math.max(a, b); ++i)
            m[files.pathAt(i)] = true
        sel = m
        selCount = Object.keys(m).length
        cursor = b
        reveal(b)
    }
    function selectAll() {
        const all = []
        for (let i = 0; i < files.count; ++i)
            all.push(files.pathAt(i))
        setSelection(all)
    }
    function clearSelection() {
        setSelection([])
        cursor = -1
        anchorRow = -1
    }
    function pruneSelection() {
        const keep = Object.keys(sel).filter(p => files.indexOf(p) >= 0)
        if (keep.length !== selCount)
            setSelection(keep)
        if (cursor >= files.count)
            cursor = files.count - 1
    }
    function reveal(row) {
        view.positionViewAtIndex(row, GridView.Contain)
    }
    // выделить (и переименовать) то, чего ещё нет в списке: папка обновится через 150 мс
    property string pendingSelect
    property bool pendingRename: false
    function checkPending() {
        if (pendingSelect && selectPath(pendingSelect)) {
            if (pendingRename)
                startRename(pendingSelect)
            pendingSelect = ""
            pendingRename = false
        }
    }
    function selectLater(path, rename) {
        pendingSelect = path
        pendingRename = rename === true
        checkPending()
    }

    // ---------- стрелки ----------
    readonly property int cols: listMode ? 1 : Math.max(1, Math.floor(gridView.width / gridView.cellWidth))
    function moveTo(row, extend) {
        if (files.count === 0)
            return
        row = Math.max(0, Math.min(files.count - 1, row))
        keyNav = true
        if (extend && anchorRow >= 0)
            selectRange(anchorRow, row)
        else
            selectOnly(row)
    }
    function moveBy(d, extend) {
        moveTo(cursor < 0 ? (d > 0 ? 0 : files.count - 1) : cursor + d, extend)
    }
    function pageRows() {
        return listMode ? Math.max(1, Math.floor(listView.height / 36))
                        : Math.max(1, Math.floor(gridView.height / gridView.cellHeight)) * cols
    }

    // ---------- открыть ----------
    function openRow(row) {
        if (files.trash || row < 0)
            return
        const it = files.get(row)
        if (it.isDir)
            go(it.path)
        else if (naomiPreview.previewable(it.path))
            desk.openPreview(fw, it.path)   // фото и видео — «Просмотр»
        else
            say("НЕЧЕМ ОТКРЫТЬ")
    }
    function openSelected() {
        const rows = []
        for (let i = 0; i < files.count; ++i)
            if (sel[files.pathAt(i)])
                rows.push(i)
        const dir = rows.find(r => files.isDirAt(r))
        openRow(dir !== undefined ? dir : rows.length ? rows[0] : -1)
    }

    // ---------- строка внизу: своё на 4 с («НЕЧЕМ ОТКРЫТЬ», «ИМЯ ЗАНЯТО») ----------
    property string note
    function say(text) {
        note = text
        noteTimer.restart()
    }
    Timer { id: noteTimer; interval: 4000; onTriggered: fw.note = "" }
    Timer { id: undoTimer; interval: 8000 }   // «ОТМЕНИТЬ» — 8 с после «в корзину»
    Connections {
        target: naomiFiles
        function onOpsChanged() {
            if (naomiFiles.canUndo && !undoTimer.running)
                undoTimer.restart()
        }
    }

    // ---------- действия ----------
    function insideSelf(paths, dest) {
        return paths.some(p => dest === p || dest.startsWith(p + "/"))
    }
    function trashSelected() {
        const paths = selectedPaths()
        if (!paths.length)
            return
        if (files.trash) {
            askForever(paths)
            return
        }
        if (files.starredPlace)
            return
        naomiFiles.trash(paths)
        undoTimer.restart()
    }
    function askForever(paths) {
        if (paths.length)
            confirm.ask(files.trash ? "trash-forever" : "forever", paths)
    }
    function copySelected(cut) {
        const paths = selectedPaths()
        if (paths.length && !files.trash)
            naomiFiles.clipboardPut(paths, cut)
    }
    function paste() {
        if (!realDir || !naomiFiles.hasClipboard)
            return
        naomiFiles.paste(files.location)
    }
    function starSelected() {
        const paths = selectedPaths()
        if (!paths.length || files.trash)
            return
        naomiFiles.setStarred(paths, !paths.every(p => naomiFiles.isStarred(p)))
    }
    function newFolder() {
        if (!realDir)
            return
        const p = naomiFiles.newFolder(files.location)
        if (p)
            selectLater(p, true)
        else
            say("НЕ ВЫШЛО")
    }
    function setWallpaper(path) {
        naomiFiles.setWallpaper(path)
        if (naomiFiles.lastError())
            say("НЕ ВЫШЛО")
    }
    function dropInto(paths, place) {
        if (!paths.length)
            return
        if (place === "trash:") {
            naomiFiles.trash(paths)
            undoTimer.restart()
        } else if (place === "starred:") {
            naomiFiles.setStarred(paths, true)
        } else if (insideSelf(paths, place)) {
            say("В САМУ СЕБЯ НЕЛЬЗЯ")
        } else if (!paths.every(p => naomiFiles.parentOf(p) === place)) {
            naomiFiles.drop(paths, place)
        }
    }

    // ---------- имя на месте (F2, «Переименовать», новая папка) ----------
    property string renaming
    property Item renameField
    function startRename(path) {
        if (!realDir)
            return
        renaming = path
        const i = files.indexOf(path)
        if (i >= 0)
            reveal(i)
    }
    function endRename(apply) {
        if (!renaming)
            return
        const from = renaming, text = renameField ? renameField.text : ""
        renameField = null
        renaming = ""
        if (apply && text) {
            const to = naomiFiles.rename(from, text)
            if (!to)
                say(naomiFiles.lastError().toUpperCase())
            else if (to !== from)
                selectLater(to)
        }
        keys.forceActiveFocus()
    }
    function renameSelected() {
        if (selCount === 1 && realDir)
            startRename(selectedPaths()[0])
    }

    // ---------- меню ----------
    function itemMenu(x, y) {
        const paths = selectedPaths()
        const n = paths.length
        if (!n)
            return
        const first = files.get(files.indexOf(paths[0]))
        let items = []
        if (files.trash) {
            items = [{ label: "Восстановить", run: () => naomiFiles.restore(paths) },
                     { label: "Удалить навсегда", run: () => fw.askForever(paths) },
                     { sep: true },
                     { label: "Очистить корзину", run: () => confirm.ask("empty", []) }]
        } else {
            items.push({ label: "Открыть", run: () => fw.openSelected() })
            if (first.isDir && n === 1) {
                items.push({ label: "Открыть в новом окне", run: () => fw.openWindow(first.path) })
                if (desk && typeof desk.openTerminalAt === "function")
                    items.push({ label: "Открыть в Терминале", run: () => desk.openTerminalAt(first.path) })
            }
            items.push({ sep: true },
                       { label: naomiFiles.isStarred(first.path) ? "Убрать из избранного" : "В избранное",
                         run: () => naomiFiles.setStarred(paths, !naomiFiles.isStarred(first.path)) })
            if (first.isDir && n === 1)
                items.push({ label: "Закрепить слева", run: () => naomiFiles.pin(first.path) })
            items.push({ sep: true },
                       { label: "Копировать", run: () => naomiFiles.clipboardPut(paths, false) },
                       { label: "Вырезать", run: () => naomiFiles.clipboardPut(paths, true) })
            if (n === 1 && !files.starredPlace)
                items.push({ label: "Переименовать", run: () => fw.startRename(first.path) })
            if (n === 1 && first.image)
                items.push({ sep: true }, { label: "Сделать фоном рабочего стола", run: () => fw.setWallpaper(first.path) })
            items.push({ sep: true }, { label: "В корзину", run: () => { naomiFiles.trash(paths); undoTimer.restart() } })
        }
        menu.popup(x, y, items)
    }
    function backgroundMenu(x, y) {
        let items = []
        if (files.trash) {
            if (files.count > 0)
                items = [{ label: "Очистить корзину", run: () => confirm.ask("empty", []) }]
        } else if (realDir) {
            if (naomiFiles.undoWhat)
                items.push({ label: "Отменить: " + naomiFiles.undoWhat.toLowerCase(), run: () => naomiFiles.undo() },
                           { sep: true })
            items.push({ label: "Новая папка", run: () => fw.newFolder() })
            if (desk && typeof desk.openTerminalAt === "function")
                items.push({ label: "Открыть папку в Терминале", run: () => desk.openTerminalAt(files.location) })
            if (naomiFiles.hasClipboard)
                items.push({ label: "Вставить", run: () => fw.paste() })
            items.push({ sep: true },
                       { label: files.showHidden ? "Спрятать скрытые" : "Показать скрытые", run: () => fw.toggleHidden() })
        }
        if (items.length)
            menu.popup(x, y, items)
    }
    function placeMenu(x, y, place, pinned) {
        let items = []
        if (place !== "starred:" && place !== "trash:")
            items.push({ label: "Открыть в новом окне", run: () => fw.openWindow(place) })
        if (pinned)
            items.push({ label: "Открепить", run: () => naomiFiles.unpin(place) })
        if (place === "trash:")
            items.push({ label: "Очистить корзину", run: () => confirm.ask("empty", []) })
        if (items.length)
            menu.popup(x, y, items)
    }
    function menuFromKeyboard() {
        const row = cursor >= 0 ? cursor : firstSelected()
        const p = row >= 0 ? itemCenter(row) : Qt.point(body.width / 2, body.height / 2)   // меню — во всё тело окна
        if (selCount > 0)
            itemMenu(p.x, p.y)
        else
            backgroundMenu(p.x, p.y)
    }
    function itemCenter(row) {   // середина плитки или строки — в точках тела окна
        const v = view
        const it = v.itemAtIndex(row)
        if (!it)
            return Qt.point(body.width / 2, body.height / 2)
        return it.mapToItem(body, it.width / 2, it.height / 2)
    }

    // ---------- нажатия по плитке и строке (общие) ----------
    property int pressRow: -1
    property point pressAt
    property bool pressWasSel: false
    property bool dragging: false
    function itemPress(row, m, p) {   // p — точка на столе
        keys.forceActiveFocus()
        keyNav = false
        const path = files.pathAt(row)
        if (renaming && renaming !== path)
            endRename(true)
        if (m.button === Qt.RightButton) {
            if (!sel[path])
                selectOnly(row)
            const q = menu.mapFromItem(desk, p.x, p.y)
            itemMenu(q.x, q.y)
            return
        }
        pressRow = row
        pressAt = p
        dragging = false
        pressWasSel = sel[path] === true
        if (m.modifiers & Qt.ControlModifier)
            toggleRow(row)
        else if ((m.modifiers & Qt.ShiftModifier) && anchorRow >= 0)
            selectRange(anchorRow, row)
        else if (!pressWasSel)
            selectOnly(row)
        else
            cursor = row
    }
    function itemMove(p) {
        if (pressRow < 0)
            return
        if (!dragging && !files.trash && Math.hypot(p.x - pressAt.x, p.y - pressAt.y) > 6 && desk) {
            if (!sel[files.pathAt(pressRow)])
                selectOnly(pressRow)
            dragging = true
            desk.dragStart(fw, selectedPaths(), files.get(pressRow))
        }
        if (dragging)
            desk.dragMove(p)
    }
    function itemRelease(row, m, p) {
        if (dragging) {
            dragging = false
            desk.dragEnd(p)
        } else if (row === pressRow && pressWasSel && selCount > 1
                   && !(m.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
            selectOnly(row)
        }
        pressRow = -1
    }

    // ---------- перетаскивание: куда бросят (зовёт стол) ----------
    property string dropKey          // что подсвечено: путь папки или «place:путь»
    function dropTarget(p) {          // p — точка в окне
        const s = fw.mapToItem(side, p.x, p.y)
        if (s.x >= 0 && s.x < side.width && s.y >= 0 && s.y < side.height) {
            const r = sideCol.childAt(s.x - sideCol.x, s.y - sideCol.y)
            return r && r.place ? { key: "place:" + r.place, place: r.place } : null
        }
        const vp = fw.mapToItem(viewport, p.x, p.y)
        if (vp.x < 0 || vp.x >= viewport.width || vp.y < 0 || vp.y >= viewport.height)
            return null
        const v = fw.mapToItem(view, p.x, p.y)
        const i = view.indexAt(v.x + view.contentX, v.y + view.contentY)
        if (i >= 0 && files.isDirAt(i) && !(desk && desk.dragPaths.includes(files.pathAt(i))))
            return { key: files.pathAt(i), place: files.pathAt(i) }
        return { key: "", place: files.location }   // в саму папку (в «Избранное» — звёздочку, в корзину — туда)
    }
    function dragOver(p) {
        const t = p ? dropTarget(p) : null
        dropKey = t ? t.key : ""
    }
    function dropAt(p, paths) {
        const t = dropTarget(p)
        dropKey = ""
        if (t)
            dropInto(paths, t.place)
    }

    // ---------- полоса: назад, вперёд, путь; плитка / список ----------
    barContent: [
        WinButton {
            x: 6; y: (fw.barH - height) / 2
            glyph: "back"
            enabled_: fw.backStack.length > 0
            onClicked: fw.goBack()
        },
        WinButton {
            x: 8 + width; y: (fw.barH - height) / 2
            glyph: "forward"
            enabled_: fw.fwdStack.length > 0
            onClicked: fw.goForward()
        },
        Item {
            id: crumbsArea
            x: 20 + 2 * DeskStyle.windowButton
            width: parent.width - x - 12
            height: fw.barH
            clip: true
            visible: !pathEdit.active
            Row {
                id: crumbsRow
                x: Math.min(0, crumbsArea.width - width)   // длинный путь — видно конец
                height: fw.barH
                spacing: 2
                Repeater {
                    model: naomiFiles.crumbs(files.location)
                    delegate: Row {
                        id: crumb
                        required property var modelData
                        required property int index
                        readonly property bool root: index === 0
                        readonly property bool current: modelData.path === files.location
                        height: fw.barH
                        spacing: 2
                        Text {   // «/» между частями
                            visible: !crumb.root
                            anchors.verticalCenter: parent.verticalCenter
                            text: "/"
                            font.family: DeskStyle.mono
                            font.pixelSize: 13
                            color: DeskStyle.dim
                        }
                        Item {
                            width: label.implicitWidth + 14
                            height: label.implicitHeight + 8
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                anchors.fill: parent
                                radius: 2
                                visible: crumbMouse.containsMouse
                                color: DeskStyle.amberA(0.10)
                            }
                            Text {
                                id: label
                                anchors.centerIn: parent
                                text: crumb.modelData.label
                                font.family: crumb.root ? DeskStyle.mono : DeskStyle.sans
                                font.pixelSize: crumb.root ? 12 : 14
                                font.letterSpacing: crumb.root ? 2 : 0
                                color: crumb.root ? DeskStyle.amber
                                     : crumbMouse.containsMouse || crumb.current ? DeskStyle.text : DeskStyle.muted
                            }
                            MouseArea {
                                id: crumbMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: fw.go(crumb.modelData.path)
                            }
                        }
                    }
                }
            }
        },
        Loader {   // Ctrl+L — путь строкой
            id: pathEdit
            x: 20 + 2 * DeskStyle.windowButton
            y: (fw.barH - height) / 2
            width: parent.width - x - 12
            height: DeskStyle.windowButton
            active: false
            onLoaded: item.input.forceActiveFocus()
            sourceComponent: Rectangle {
                property alias input: input
                color: DeskStyle.surface
                border.width: 1
                border.color: DeskStyle.amberA(0.35)
                radius: 2
                TextInput {
                    id: input
                    x: 8
                    width: parent.width - 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: fw.realDir ? files.location : ""
                    font.family: DeskStyle.sans
                    font.pixelSize: 14
                    color: DeskStyle.text
                    selectionColor: DeskStyle.amberA(0.35)
                    selectedTextColor: DeskStyle.text
                    selectByMouse: true
                    clip: true
                    cursorDelegate: Rectangle { width: 1; color: DeskStyle.amber }
                    Component.onCompleted: selectAll()
                    Text {
                        visible: !input.text
                        text: "путь к папке"
                        font: input.font
                        color: DeskStyle.dim
                    }
                    Keys.onEscapePressed: keys.forceActiveFocus()
                    Keys.onReturnPressed: {
                        const t = text.trim().replace(/^~(?=\/|$)/, naomiFiles.home)
                        if (naomiFiles.isDir(t)) {
                            fw.go(t)
                            keys.forceActiveFocus()
                        } else {
                            fw.say("НЕТ ТАКОЙ ПАПКИ")
                        }
                    }
                    onActiveFocusChanged: if (!activeFocus) Qt.callLater(() => { pathEdit.active = false })
                }
            }
        }
    ]
    barTools: [
        WinButton {
            visible: fw.realDir && desk && typeof desk.openTerminalAt === "function"
            y: (fw.barH - height) / 2
            glyph: "term"
            onClicked: desk.openTerminalAt(files.location)
        },
        Item { width: 2; height: 1 },
        WinButton {
            y: (fw.barH - height) / 2
            glyph: fw.listMode ? "grid" : "list"
            onClicked: fw.setMode(fw.listMode ? "grid" : "list")
        },
        Item {
            width: 6 + 1 + 4
            height: fw.barH
            Rectangle {
                x: 6; y: 11
                width: 1; height: fw.barH - 22
                color: DeskStyle.amberA(DeskStyle.line)
            }
        }
    ]

    // ---------- тело: клавиши ----------
    // фокус — когда окно активно (Desktop.qml, raise); своё focus: true отдало бы клавиши запасу — невидимому окну,
    // которое ждёт за краем стола, пока окон «Файлов» нет (Ctrl+Shift+N на пустом столе — «Новая папка» дома)
    FocusScope {
        id: keys
        anchors.fill: parent
        Keys.onPressed: (e) => fw.key(e)
    }
    onActiveChanged: if (active && !renaming && !pathEdit.active) keys.forceActiveFocus()

    // ---------- слева: места (фон — окна: у края окна ничего своего не рисуется) ----------
    Item {
        id: side
        width: 232
        height: parent.height - statusBar.height
        Rectangle {
            anchors.right: parent.right
            width: 1; height: parent.height
            color: DeskStyle.amberA(DeskStyle.line)
        }
        Column {
            id: sideCol
            y: 16
            width: parent.width
            SidePlate { num: "01"; title: "МЕСТА" }
            Item { width: 1; height: 6 }
            PlaceRow { label: "Дом"; place: naomiFiles.home }
            PlaceRow { label: "Избранное"; place: "starred:"; count: naomiFiles.starred.length ? String(naomiFiles.starred.length) : "" }
            PlaceRow { label: "Корзина"; place: "trash:"; count: naomiFiles.trashCount ? String(naomiFiles.trashCount) : "" }
            Item { width: 1; height: 18 }
            SidePlate { num: "02"; title: "СВОИ" }
            Item { width: 1; height: 6 }
            Repeater {
                model: naomiFiles.bookmarks
                delegate: PlaceRow {
                    required property var modelData
                    label: modelData.label
                    place: modelData.path
                    pinned: true
                }
            }
            Text {
                visible: naomiFiles.bookmarks.length === 0
                x: 28
                topPadding: 2
                text: "правый клик по папке —\nзакрепить слева"
                font.family: DeskStyle.mono
                font.pixelSize: 11
                color: DeskStyle.dim
            }
        }
    }

    // ---------- справа: папка ----------
    Item {
        id: viewport
        x: side.width
        width: parent.width - x
        height: parent.height - statusBar.height
        clip: true

        // пусто и мимо плиток: щелчок — снять выделение, тянуть — рамка, правый — меню папки; колесо — прокрутка
        MouseArea {
            id: bgMouse
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property point from        // в точках содержимого
            property var base: ({})
            property bool banding: false
            onPressed: (m) => {
                keys.forceActiveFocus()
                fw.keyNav = false
                fw.endRename(true)
                if (m.button === Qt.RightButton) {
                    fw.clearSelection()
                    const q = mapToItem(menu, m.x, m.y)
                    fw.backgroundMenu(q.x, q.y)
                    return
                }
                const add = m.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)
                if (!add)
                    fw.clearSelection()
                base = add ? Object.assign({}, fw.sel) : {}
                const c = toContent(m.x, m.y)
                from = c
                banding = false
            }
            onPositionChanged: (m) => {
                if (!(m.buttons & Qt.LeftButton))
                    return
                const c = toContent(m.x, m.y)
                if (!banding && Math.hypot(c.x - from.x, c.y - from.y) < 4)
                    return
                banding = true
                band.to = c
                band.update()
                autoScroll.dy = m.y < 0 ? -12 : m.y > height ? 12 : 0
            }
            onReleased: { banding = false; autoScroll.dy = 0 }
            // колесо и трекпад (WheelHandler столу событий колеса не получает — проверено, MouseArea получает)
            onWheel: (e) => fw.scrollBy(e.pixelDelta.y !== 0 ? -e.pixelDelta.y : -e.angleDelta.y / 120 * 60)
            function toContent(x, y) {   // точка вида → точка содержимого
                const v = fw.view
                const p = mapToItem(v, x, y)
                return Qt.point(p.x + v.contentX, p.y + v.contentY)
            }
        }
        Timer {
            id: autoScroll
            property real dy: 0
            interval: 16
            repeat: true
            running: dy !== 0 && bgMouse.banding
            onTriggered: { fw.scrollBy(dy); band.to.y += dy; band.update() }
        }

        // ---------- плитка ----------
        GridView {
            id: gridView
            visible: !fw.listMode
            x: 16
            width: parent.width - 32
            height: parent.height
            topMargin: 14
            bottomMargin: 14
            interactive: false
            // выделение и курсор у окна свои (sel, cursor); текущего элемента Qt нет — и его невидимой подсветки,
            // которая едет за ним сама (у списка — 400 точек/с: ~2,4 с после открытия стол рисовал 75 к/с впустую)
            currentIndex: -1
            highlightFollowsCurrentItem: false
            // плитки и строки — заново не создаются: открыли папку — те же элементы получают новые файлы (было — каждая
            // папка заново ~15 строк по ~12 элементов с двумя фигурами: 30–45 мс в потоке интерфейса, прогон стола
            // 19 ноября 2026, docs/FILES.md, «Растягивание и папки»)
            reuseItems: true
            model: fw.listMode ? null : files
            readonly property int columns: Math.max(1, Math.floor(width / 128))
            cellWidth: Math.floor(width / columns)
            cellHeight: 132
            delegate: Item {
                id: tile
                required property int index
                required property string name
                required property string path
                required property bool isDir
                required property string ext
                required property bool starred
                required property bool image
                width: gridView.cellWidth
                height: gridView.cellHeight
                readonly property bool selected: fw.sel[path] === true
                readonly property bool editing: fw.renaming === path
                Rectangle {
                    x: (parent.width - width) / 2
                    y: 2
                    width: Math.min(parent.width - 4, 124)
                    height: parent.height - 4
                    radius: 3
                    color: fw.dropKey === tile.path ? DeskStyle.amberA(0.24) : tile.selected ? DeskStyle.amberA(0.14)
                         : tileMouse.containsMouse ? DeskStyle.amberA(0.07) : "transparent"
                    border.width: tile.selected || (fw.keyNav && fw.cursor === tile.index) ? 1 : 0
                    border.color: DeskStyle.amberA(tile.selected ? 0.45 : 0.55)
                }
                FileIcon {
                    x: (parent.width - width) / 2
                    y: 2 + 8
                    dir: tile.isDir
                    ext: tile.ext
                    starred: tile.starred
                    image: tile.image
                    path: tile.path
                }
                TextMetrics {   // в две строки — середина имени многоточием (видно и начало, и тип)
                    id: fit
                    font: nameText.font
                    elide: Text.ElideMiddle
                    elideWidth: 2 * 116 - 20
                    text: tile.name
                }
                Text {
                    id: nameText
                    visible: !tile.editing
                    x: (parent.width - width) / 2
                    y: 2 + 8 + 74 + 6
                    width: 116
                    text: fit.elidedText
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: DeskStyle.sans
                    font.pixelSize: 13
                    color: DeskStyle.text
                }
                MouseArea {
                    id: tileMouse
                    x: (parent.width - width) / 2
                    y: 2
                    width: Math.min(parent.width - 4, 124)
                    height: parent.height - 4
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (m) => fw.itemPress(tile.index, m, mapToItem(fw.desk, m.x, m.y))
                    onPositionChanged: (m) => { if (pressed) fw.itemMove(mapToItem(fw.desk, m.x, m.y)) }
                    onReleased: (m) => fw.itemRelease(tile.index, m, mapToItem(fw.desk, m.x, m.y))
                    onDoubleClicked: (m) => { if (m.button === Qt.LeftButton) fw.openRow(tile.index) }
                }
                Loader {
                    active: tile.editing
                    x: (parent.width - width) / 2
                    y: nameText.y - 3
                    width: 120
                    sourceComponent: RenameField { tileStyle: true; name: tile.name; isDir: tile.isDir }
                }
            }
        }

        // ---------- список ----------
        Item {
            id: listBox
            visible: fw.listMode
            x: 16
            width: parent.width - 32
            height: parent.height
            readonly property real starW: files.trash ? 0 : 52
            readonly property real nameW: width - 170 - 170 - 110 - starW
            // Столбцы: ширина имени — от окна, остальные постоянные (ТИП 170, СОЗДАН 170, РАЗМЕР 110). Модель шапки —
            // постоянный список, место — от nameW: было — массив объектов от ширины, и при каждом шаге растягивания
            // окна Repeater шапки пересоздавал все 4 столбца (со значком-фигурой), а строки пересчитывали привязки к
            // новому массиву (прогон стола 19 ноября 2026, docs/FILES.md, «Растягивание»)
            readonly property var colKeys: ["name", "type", "date", "size"]
            readonly property var colW: [0, 170, 170, 110]   // [0] — имя, его ширина — nameW
            readonly property var colOff: [0, 0, 170, 340]   // от nameW
            Item {
                id: header
                width: parent.width
                height: 6 + 28 + 1
                Repeater {
                    model: listBox.colKeys
                    delegate: Item {
                        id: col
                        required property string modelData
                        required property int index
                        readonly property string title: index === 0 ? "ИМЯ" : index === 1 ? "ТИП"
                                                      : index === 2 ? (files.trash ? "УДАЛЁН" : "СОЗДАН") : "РАЗМЕР"
                        x: index === 0 ? 0 : listBox.nameW + listBox.colOff[index]
                        y: 6
                        width: index === 0 ? listBox.nameW : listBox.colW[index]
                        height: 28
                        Rectangle {
                            anchors.fill: parent
                            visible: colMouse.containsMouse
                            color: DeskStyle.amberA(0.06)
                        }
                        Text {
                            id: colTitle
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: col.title
                            font.family: DeskStyle.mono
                            font.pixelSize: 11
                            font.letterSpacing: 2
                            color: colMouse.containsMouse ? DeskStyle.text : DeskStyle.dim
                        }
                        Glyph {
                            visible: files.sortKey === col.modelData
                            x: colTitle.x + colTitle.implicitWidth + 4 + 6
                            y: parent.height / 2
                            kind: files.descending ? "down" : "up"
                            line: 1.4
                            color: DeskStyle.amber
                        }
                        MouseArea {
                            id: colMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: fw.setSort(col.modelData)
                        }
                    }
                }
                Rectangle {
                    y: parent.height - 1
                    width: parent.width; height: 1
                    color: DeskStyle.amberA(DeskStyle.line)
                }
            }
            ListView {
                id: listView
                y: header.height
                width: parent.width
                height: parent.height - y
                topMargin: 6
                bottomMargin: 14
                clip: true
                interactive: false
                currentIndex: -1                     // как у плитки: без текущего и его подсветки
                highlightFollowsCurrentItem: false
                reuseItems: true                     // как у плитки: строки переиспользуются
                model: fw.listMode ? files : null
                delegate: Item {
                    id: row
                    required property int index
                    required property string name
                    required property string path
                    required property bool isDir
                    required property string ext
                    required property string type
                    required property string dateText
                    required property string sizeText
                    required property bool starred
                    required property bool image
                    width: listView.width
                    height: 36
                    readonly property bool selected: fw.sel[path] === true
                    readonly property bool editing: fw.renaming === path
                    Rectangle {
                        y: 1
                        width: parent.width; height: 34
                        radius: 3
                        color: fw.dropKey === row.path ? DeskStyle.amberA(0.24) : row.selected ? DeskStyle.amberA(0.14)
                             : rowMouse.containsMouse ? DeskStyle.amberA(0.07) : "transparent"
                        border.width: row.selected || (fw.keyNav && fw.cursor === row.index) ? 1 : 0
                        border.color: DeskStyle.amberA(row.selected ? 0.45 : 0.55)
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: (m) => fw.itemPress(row.index, m, mapToItem(fw.desk, m.x, m.y))
                        onPositionChanged: (m) => { if (pressed) fw.itemMove(mapToItem(fw.desk, m.x, m.y)) }
                        onReleased: (m) => fw.itemRelease(row.index, m, mapToItem(fw.desk, m.x, m.y))
                        onDoubleClicked: (m) => { if (m.button === Qt.LeftButton) fw.openRow(row.index) }
                    }
                    FileIcon {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        k: 0.34
                        dir: row.isDir
                        ext: row.ext
                        image: row.image
                        path: row.path
                    }
                    Text {
                        id: rowName
                        visible: !row.editing
                        x: 12 + 33 + 10
                        width: listBox.nameW - x - 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.name
                        elide: Text.ElideMiddle
                        font.family: DeskStyle.sans
                        font.pixelSize: 14
                        color: DeskStyle.text
                    }
                    Loader {
                        active: row.editing
                        x: rowName.x - 7
                        width: listBox.nameW - x - 8
                        anchors.verticalCenter: parent.verticalCenter
                        sourceComponent: RenameField { name: row.name; isDir: row.isDir }
                    }
                    // ТИП, СОЗДАН, РАЗМЕР — три текста, а не Repeater по массиву: строку переиспользуют для другого
                    // файла, и Repeater с новым массивом пересоздавал бы свои тексты (папка — 30 мс, выборки стека)
                    Text {
                        x: listBox.nameW + listBox.colOff[1] + 12
                        width: listBox.colW[1] - 24
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.type
                        elide: Text.ElideRight
                        font.family: DeskStyle.sans
                        font.pixelSize: 13
                        color: DeskStyle.muted
                    }
                    Text {
                        x: listBox.nameW + listBox.colOff[2] + 12
                        width: listBox.colW[2] - 24
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.dateText
                        elide: Text.ElideRight
                        font.family: DeskStyle.sans
                        font.pixelSize: 13
                        color: DeskStyle.muted
                    }
                    Text {
                        x: listBox.nameW + listBox.colOff[3] + 12
                        width: listBox.colW[3] - 24
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.sizeText
                        elide: Text.ElideRight
                        font.family: DeskStyle.sans
                        font.pixelSize: 13
                        color: DeskStyle.muted
                    }
                    Item {   // звёздочка: в избранное и обратно
                        visible: !files.trash
                        x: parent.width - 52 + (52 - 24) / 2
                        width: 24; height: 24
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            visible: starMouse.containsMouse
                            color: DeskStyle.amberA(0.10)
                        }
                        Glyph {
                            x: 12; y: 12
                            kind: "star"
                            unit: 8 / 6
                            fill: row.starred
                            color: row.starred || starMouse.containsMouse ? DeskStyle.amber : fw.dimA(0.55)
                        }
                        MouseArea {
                            id: starMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: naomiFiles.setStarred([row.path], !row.starred)
                        }
                    }
                }
            }
        }

        // рамка выделения
        Rectangle {
            id: band
            property point to
            visible: bgMouse.banding
            color: DeskStyle.amberA(0.12)
            border.width: 1
            border.color: DeskStyle.amberA(0.6)
            function update() {
                const v = fw.view
                const x0 = Math.min(bgMouse.from.x, to.x), x1 = Math.max(bgMouse.from.x, to.x)
                const y0 = Math.min(bgMouse.from.y, to.y), y1 = Math.max(bgMouse.from.y, to.y)
                const tl = v.mapToItem(viewport, x0 - v.contentX, y0 - v.contentY)
                x = tl.x; y = tl.y; width = x1 - x0; height = y1 - y0
                const m = Object.assign({}, bgMouse.base)
                for (let i = 0; i < files.count; ++i) {
                    let rx, ry, rw, rh
                    if (fw.listMode) {
                        rx = 0; ry = i * 36 + 1; rw = v.width; rh = 34
                    } else {
                        const cw = gridView.cellWidth, ch = gridView.cellHeight
                        rw = Math.min(cw - 4, 124); rh = ch - 4
                        rx = (i % gridView.columns) * cw + (cw - rw) / 2; ry = Math.floor(i / gridView.columns) * ch + 2
                    }
                    if (rx < x1 && rx + rw > x0 && ry < y1 && ry + rh > y0)
                        m[files.pathAt(i)] = true
                }
                fw.sel = m
                fw.selCount = Object.keys(m).length
            }
        }

        // полоса прокрутки: янтарь .30, 4 точки
        Rectangle {
            readonly property var v: fw.view
            readonly property real total: v.contentHeight + v.topMargin + v.bottomMargin
            visible: total > v.height + 1
            x: parent.width - 6
            width: 4
            radius: 2
            color: DeskStyle.amberA(0.30)
            readonly property point at: v.mapToItem(viewport, 0, 0)
            height: Math.max(24, v.height * v.height / total)
            y: at.y + (v.height - height) * (v.contentY + v.topMargin - v.originY) / Math.max(1, total - v.height)
        }

        // пусто
        Text {
            anchors.centerIn: parent
            visible: files.count === 0
            text: !files.readable ? "НЕТ ДОСТУПА" : files.trash ? "КОРЗИНА ПУСТА"
                : files.starredPlace ? "В ИЗБРАННОМ ПУСТО" : "ПАПКА ПУСТА"
            font.family: DeskStyle.mono
            font.pixelSize: 13
            font.letterSpacing: 3
            color: DeskStyle.dim
        }
    }
    function scrollBy(dy) {
        const v = view
        const min = v.originY - v.topMargin
        const max = Math.max(min, v.originY + v.contentHeight + v.bottomMargin - v.height)
        v.contentY = Math.max(min, Math.min(max, v.contentY + dy))
    }

    // ---------- строка внизу ----------
    Item {
        id: statusBar
        y: parent.height - height
        width: parent.width
        height: 27
        Rectangle {
            width: parent.width; height: 1
            color: DeskStyle.amberA(DeskStyle.line)
        }
        readonly property real selSize: {
            let s = 0
            for (const p of Object.keys(fw.sel)) {
                const i = files.indexOf(p)
                if (i >= 0 && !files.isDirAt(i))
                    s += files.get(i).size
            }
            return s
        }
        function sizeText(b) {
            const units = ["Б", "КБ", "МБ", "ГБ", "ТБ"]
            let u = 0
            while (b >= 1000 && u < units.length - 1) { b /= 1000; ++u }
            return (u === 0 ? String(b) : b.toFixed(b < 10 ? 1 : 0).replace(".", ",")) + " " + units[u]
        }
        StatusText {
            id: countText
            x: 14
            color: DeskStyle.muted
            text: fw.selCount === 0 ? files.count + " " + fw.plural(files.count, "ОБЪЕКТ", "ОБЪЕКТА", "ОБЪЕКТОВ")
                : "ВЫДЕЛЕНО " + fw.selCount + " ИЗ " + files.count + (statusBar.selSize > 0 ? " · " + statusBar.sizeText(statusBar.selSize) : "")
        }
        StatusText {
            anchors.right: statusAction.visible ? statusAction.left : parent.right
            anchors.rightMargin: statusAction.visible ? 12 : 14
            width: Math.max(0, (statusAction.visible ? statusAction.x - 12 : parent.width - 14) - countText.x - countText.implicitWidth - 12)
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            color: DeskStyle.muted
            text: naomiFiles.status || fw.note
        }
        StatusText {
            id: statusAction
            readonly property bool busy: naomiFiles.busy
            visible: busy || (naomiFiles.canUndo && undoTimer.running)
            anchors.right: parent.right
            anchors.rightMargin: 14 + 6
            text: busy ? "ОСТАНОВИТЬ" : "ОТМЕНИТЬ"
            color: actionMouse.containsMouse ? DeskStyle.text : DeskStyle.amber
            MouseArea {
                id: actionMouse
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                onClicked: {
                    if (naomiFiles.busy)
                        naomiFiles.cancel()
                    else {
                        naomiFiles.undo()
                        undoTimer.stop()
                    }
                }
            }
        }
    }

    // ---------- «Просмотр» (пробел; docs/DESKTOP.md, «Просмотр») ----------
    // Окно просмотра — стола (Desktop.qml, preview/PreviewWindow.qml), как naomi-preview у НаомиОС: пока оно открыто
    // отсюда, смена выделения показывает там выделенный файл; стрелки в нём двигают выделение здесь — по фото и видео,
    // остальное пропуская (previewStep); Alt+Backspace в нём — сюда же, в корзину с «Отменить» (previewTrash).
    onCursorChanged: if (cursor >= 0 && desk) desk.previewFollow(fw, files.pathAt(cursor))
    function previewNeighbour(path, d) {   // следующее фото или видео от path (d = ±1), "" — дальше нет
        for (let i = files.indexOf(path) + d; i >= 0 && i < files.count; i += d)
            if (!files.isDirAt(i) && naomiPreview.previewable(files.pathAt(i)))
                return files.pathAt(i)
        return ""
    }
    function previewStep(d) {
        const p = previewNeighbour(cursor >= 0 ? files.pathAt(cursor) : "", d)
        if (p) {
            keyNav = true
            selectPath(p)
        }
        return p
    }
    function previewTrash(path) {   // как у НаомиОС: убирают, если он выделен один
        if (files.trash || files.starredPlace || selCount !== 1 || !sel[path])
            return false
        naomiFiles.trash([path])
        undoTimer.restart()
        return true
    }
    function focusKeys() { keys.forceActiveFocus() }

    // ---------- вопрос «навсегда» ----------
    Item {
        id: confirm
        anchors.fill: parent
        visible: kind !== ""
        property string kind            // forever, trash-forever, empty
        property var paths: []
        property int focusIndex: 0      // 0 — ОТМЕНА (Enter сразу — отмена), 1 — УДАЛИТЬ
        function ask(k, p) {
            paths = p
            kind = k
            focusIndex = 0
            confirmKeys.forceActiveFocus()
        }
        function answer(yes) {
            const k = kind, p = paths
            kind = ""
            paths = []
            keys.forceActiveFocus()
            if (!yes)
                return
            if (k === "empty")
                naomiFiles.emptyTrash()
            else
                naomiFiles.deleteForever(p)
        }
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; hoverEnabled: true }
        FocusScope {
            id: confirmKeys
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Escape)
                    confirm.answer(false)
                else if (e.key === Qt.Key_Tab || e.key === Qt.Key_Backtab || e.key === Qt.Key_Left || e.key === Qt.Key_Right)
                    confirm.focusIndex = 1 - confirm.focusIndex
                else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Space)
                    confirm.answer(confirm.focusIndex === 1)
                e.accepted = true
            }
        }
        Rectangle {
            id: box
            width: 400
            height: boxCol.implicitHeight + 36
            anchors.centerIn: parent
            color: DeskStyle.surface
            border.width: 1
            border.color: DeskStyle.amberA(0.30)
            Rectangle { width: 3; height: parent.height; color: DeskStyle.bad }
            Column {
                id: boxCol
                x: 22; y: 18
                width: parent.width - 44
                Item {
                    width: parent.width
                    height: 14
                    Text {
                        id: bang
                        anchors.verticalCenter: parent.verticalCenter
                        text: "!!"
                        font.family: DeskStyle.mono
                        font.pixelSize: 11
                        color: DeskStyle.dim
                    }
                    Text {
                        id: kicker
                        x: bang.implicitWidth + 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: "НАВСЕГДА"
                        font.family: DeskStyle.mono
                        font.pixelSize: 11
                        font.letterSpacing: 2.4
                        color: DeskStyle.bad
                    }
                    Rectangle {
                        x: kicker.x + kicker.implicitWidth + 8
                        width: Math.max(0, parent.width - x)
                        height: 1
                        anchors.verticalCenter: parent.verticalCenter
                        color: DeskStyle.amberA(DeskStyle.line)
                    }
                }
                Item { width: 1; height: 14 }
                Text {
                    width: parent.width
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    text: {
                        const n = confirm.paths.length
                        if (confirm.kind === "empty")
                            return "Очистить корзину?"
                        if (n === 1) {
                            const i = files.indexOf(confirm.paths[0])
                            return "Удалить «" + (i >= 0 ? files.get(i).name : confirm.paths[0].split("/").pop()) + "» навсегда?"
                        }
                        return "Удалить навсегда " + n + " " + fw.plural(n, "объект", "объекта", "объектов") + "?"
                    }
                    font.family: DeskStyle.sans
                    font.pixelSize: 16
                    color: DeskStyle.text
                }
                Item { width: 1; height: 4 }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: confirm.kind === "empty" ? "Всё, что в ней лежит, удалится — вернуть будет нельзя."
                                                   : "В корзину не попадёт — вернуть будет нельзя."
                    font.family: DeskStyle.sans
                    font.pixelSize: 13
                    color: DeskStyle.muted
                }
                Item { width: 1; height: 20 }
                Row {
                    anchors.right: parent.right
                    spacing: 10
                    Repeater {
                        model: [{ label: "ОТМЕНА", danger: false },
                                { label: confirm.kind === "empty" ? "ОЧИСТИТЬ" : "УДАЛИТЬ", danger: true }]
                        delegate: Rectangle {
                            id: cb
                            required property var modelData
                            required property int index
                            readonly property bool focused: confirm.focusIndex === index
                            width: cbText.implicitWidth + 32
                            height: cbText.implicitHeight + 12
                            radius: 2
                            color: cbMouse.containsMouse || (focused && modelData.danger)
                                   ? (modelData.danger ? fw.badA(0.14) : DeskStyle.amberA(0.10)) : "transparent"
                            border.width: 1
                            border.color: modelData.danger ? (focused ? DeskStyle.bad : fw.badA(0.45))
                                                           : (focused ? DeskStyle.amber : DeskStyle.amberA(0.25))
                            Text {
                                id: cbText
                                anchors.centerIn: parent
                                text: cb.modelData.label
                                font.family: DeskStyle.mono
                                font.pixelSize: 12
                                font.letterSpacing: 2
                                color: cb.modelData.danger ? DeskStyle.bad
                                     : cbMouse.containsMouse || cb.focused ? DeskStyle.text : DeskStyle.muted
                            }
                            MouseArea {
                                id: cbMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: confirm.answer(cb.modelData.danger)
                            }
                        }
                    }
                }
            }
        }
    }

    FilesMenu {
        id: menu
        onDone: keys.forceActiveFocus()
    }

    // ---------- клавиши (как on_key в files-window.c НаомиОС) ----------
    property string typed
    property double typedAt: 0
    function key(e) {
        const ctrl = (e.modifiers & Qt.ControlModifier) !== 0, alt = (e.modifiers & Qt.AltModifier) !== 0
        const shift = (e.modifiers & Qt.ShiftModifier) !== 0, none = !ctrl && !alt && !shift
        e.accepted = true
        if (menu.shown) {
            if (e.key === Qt.Key_Escape)
                menu.close()
            return
        }
        switch (e.key) {
        case Qt.Key_Space:   // «Просмотр»: открыть выделенное, открыт — закрыть
            if (none) {
                const row = cursor >= 0 ? cursor : firstSelected()
                desk.togglePreview(fw, row >= 0 ? files.pathAt(row) : "")
                return
            }
            break
        case Qt.Key_Backspace:
            if (none) { goUp(); return }
            if (alt) { trashSelected(); return }
            break
        case Qt.Key_Delete:
            if (shift) askForever(selectedPaths())
            else trashSelected()
            return
        case Qt.Key_Left:
            if (alt) goBack(); else moveBy(-1, shift)
            return
        case Qt.Key_Right:
            if (alt) goForward(); else moveBy(1, shift)
            return
        case Qt.Key_Up:
            if (alt) goUp(); else moveBy(-cols, shift)
            return
        case Qt.Key_Down:
            if (!alt) moveBy(cols, shift)
            return
        case Qt.Key_Home:
            if (alt) go(naomiFiles.home); else moveTo(0, shift)
            return
        case Qt.Key_End:
            moveTo(files.count - 1, shift)
            return
        case Qt.Key_PageUp:
            moveBy(-pageRows(), shift)
            return
        case Qt.Key_PageDown:
            moveBy(pageRows(), shift)
            return
        case Qt.Key_Return:
        case Qt.Key_Enter:
            openSelected()
            return
        case Qt.Key_F2:
            renameSelected()
            return
        case Qt.Key_Menu:
            menuFromKeyboard()
            return
        case Qt.Key_F10:
            if (shift) { menuFromKeyboard(); return }
            break
        case Qt.Key_F5:
            files.reload()
            return
        case Qt.Key_Escape:
            clearSelection()
            return
        }
        if (ctrl && !alt && !shift) {
            switch (e.key) {
            case Qt.Key_Z: naomiFiles.undo(); return
            case Qt.Key_C: copySelected(false); return
            case Qt.Key_X: copySelected(true); return
            case Qt.Key_V: paste(); return
            case Qt.Key_A: selectAll(); return
            case Qt.Key_H: toggleHidden(); return
            case Qt.Key_D: starSelected(); return
            case Qt.Key_L: pathEdit.active = true; return
            case Qt.Key_N: openWindow(files.location); return
            case Qt.Key_W: close(); return
            case Qt.Key_R: files.reload(); return
            case Qt.Key_1: setMode("grid"); return
            case Qt.Key_2: setMode("list"); return
            }
        }
        if (ctrl && shift && !alt && e.key === Qt.Key_N) {
            newFolder()
            return
        }
        // буквы — к файлу с таким началом имени (набор за 0,9 с)
        if (!ctrl && !alt && e.text.length === 1 && e.text !== " " && e.text.charCodeAt(0) >= 32) {
            const now = Date.now()
            typed = now - typedAt > 900 ? e.text : typed + e.text
            typedAt = now
            const i = files.find(typed, 0)
            if (i >= 0)
                selectOnly(i)
            return
        }
        e.accepted = false
    }
}
