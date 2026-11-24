// Разделы слева в «Настройках» — «Шкала» (вариант 08, выбран пользователем 21 ноября 2026 из десяти; docs/DESKTOP.md,
// «Настройки»): разделы — деления на одной оси, как у ползунка «Размер» на странице «Экран». Раздел — длинное деление и
// название моно-капсом, подраздел — короткое и строчными sans со сдвигом. Открытый — бегунок на оси (едет к нему
// 220 мс, без отскока) и название янтарём; раздел открытого подраздела — светлым, под мышью — светлым. Узко — названия
// обрезаются многоточием.
import QtQuick
import ".."

Item {
    id: nav
    property string page
    signal pick(string id)
    // разделы и подразделы — по порядку; main — раздел (под ним — его подразделы)
    readonly property var allItems: [
        { id: "screen", name: "Экран", main: true }, { id: "wall", name: "Обои" }, { id: "lock", name: "Без дока" },
        { id: "mouse", name: "Мышь", main: true }, { id: "more", name: "Подробно" },
        { id: "touch", name: "Трекпад", main: true },
        { id: "battery", name: "Батарея", main: true }, { id: "health", name: "Здоровье" },
        { id: "pad", name: "Геймпад", main: true }]
    readonly property var items: {
        if (!DeskStyle.tablet)
            return allItems
        const tabletItems = allItems.filter(it => !["lock", "touch"].includes(it.id))
        tabletItems.splice(2, 0, { id: "interface", name: "Интерфейс", main: true })
        tabletItems.push({ id: "system", name: "Система", main: true })
        if (naomiSystemTools.vpnAvailable)
            tabletItems.push({ id: "vpn", name: "VPN", main: true })
        return tabletItems
    }
    readonly property int mainH: DeskStyle.tablet ? 44 : 34
    readonly property int subH: DeskStyle.tablet ? 40 : 28
    readonly property int groupGap: 6
    readonly property real axisX: 14.5                    // ось — середина делений
    // где строка: сверху вниз, между разделами — groupGap
    readonly property var rows: {
        const out = []
        let y = 0, sec = ""
        for (const it of items) {
            if (it.main && out.length)
                y += groupGap
            if (it.main)
                sec = it.id
            const h = it.main ? mainH : subH
            out.push({ id: it.id, name: it.name, main: !!it.main, sec: sec, y: y, h: h })
            y += h
        }
        return out
    }
    readonly property real usedHeight: rows.length ? rows[rows.length - 1].y + rows[rows.length - 1].h : 0
    readonly property string section: { const r = rows.find(o => o.id === page); return r ? r.sec : "" }
    function center(id) { const r = rows.find(o => o.id === id); return r ? r.y + r.h / 2 : 0 }
    implicitHeight: usedHeight

    Rectangle {   // ось: от первого деления до последнего
        x: nav.axisX - 0.5
        y: nav.center(nav.items[0].id)
        width: 1
        height: nav.center(nav.items[nav.items.length - 1].id) - y
        color: DeskStyle.amberA(DeskStyle.line)
    }
    Repeater {
        model: nav.rows
        delegate: Item {
            id: row
            required property var modelData
            readonly property bool on: nav.page === modelData.id
            readonly property bool parentOn: modelData.main && !on && nav.section === modelData.id
            readonly property bool hovered: area.containsMouse
            y: modelData.y
            width: nav.width
            height: modelData.h
            Rectangle {   // деление
                x: row.modelData.main ? nav.axisX - 7.5 : nav.axisX - 3
                y: (row.height - height) / 2
                width: row.modelData.main ? 15 : 6
                height: row.modelData.main ? 1.5 : 1
                color: Qt.rgba(125 / 255, 120 / 255, 110 / 255, 0.85)
            }
            Text {
                x: row.modelData.main ? 38 : 46
                width: row.width - x
                height: row.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: row.modelData.main ? row.modelData.name.toUpperCase() : row.modelData.name
                font.family: row.modelData.main ? DeskStyle.mono : DeskStyle.sans
                font.pixelSize: row.modelData.main ? 13 : 14
                font.letterSpacing: row.modelData.main ? 13 * 0.16 : 0
                color: row.on ? DeskStyle.amber : row.parentOn || row.hovered ? DeskStyle.text : DeskStyle.muted
            }
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                onClicked: nav.pick(row.modelData.id)
            }
        }
    }
    Rectangle {   // бегунок — как у ползунка «Размер», лёжа
        x: nav.axisX - 8
        y: nav.center(nav.page) - 5
        width: 16
        height: 10
        radius: 3
        color: DeskStyle.bg
        border.width: 1.5
        border.color: DeskStyle.amber
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }
}
