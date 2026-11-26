// Батарея на столе (docs/DESKTOP.md, «Батарея») — общее для значка на панели и окна батареи: заряд, зарядка, ток и
// прогноз «сколько ещё проработает». Прогноз — запас датчика заряда (chargeMah) ÷ ток, сглаженный за минуту (ток
// датчика верен, но шумит: σ ~220 мА, всплески до 1,4 А — docs/NAOMID.md); вверх ток — заряд, вниз — разряд. Ток
// спрашивается у naomid (Sample): раз в секунду, пока окно открыто, и раз в 5 с, пока от батареи (значок показывает
// время); на зарядке с закрытым окном и когда стол не смотрят — не спрашивается. История — naomid History: при открытии
// окна, при смене «24 Ч · 7 ДН» и раз в минуту, пока открыто.
import QtQuick

QtObject {
    id: b
    property bool live: false          // стол смотрят
    property bool open: false          // окно батареи открыто
    property string range: "day"       // окно: day — сутки по 15 мин, week — неделя по часу

    readonly property int percent: naomiPhone.battery.percent
    readonly property bool plugged: naomiPhone.battery.charging   // зарядка подключена (на доке — и когда не успевает)
    readonly property bool charging: naomiPhone.battery.state === "charging"   // ядро заряжает (не «полная», не «ждёт»)
    readonly property bool low: !plugged && percent <= 20

    property var sample: ({})          // последний Sample() службы
    property real currentMa: 0         // сглаженный ток: + в батарею, − из неё
    property bool known: false         // ток уже есть
    property real takenAt: 0
    property bool reseed: false        // зарядку подключили или отключили — начать сглаживание с нынешнего тока
    property var history: null         // History() за range

    // прогноз: kind — left (от батареи: осталось), short (ядро заряжает, а ток из батареи — док не успевает: зарядка
    // не покрывает расход), full (до полной), hold (на зарядке и держится; «полная» и «ждёт подзарядки» с током из
    // батареи — тоже: так ядро разряжает её до порога само), "" — тока ещё нет; hours — часы, −1 — не считается
    readonly property var forecast: {
        if (!known)
            return { kind: "", hours: -1 }
        const I = currentMa, q = sample.chargeMah || 0
        if (I < -15 && (!plugged || charging))
            return { kind: plugged ? "short" : "left", hours: q / -I }
        if (I > 15 && percent < 100)   // простым делением; в конце заряд идёт медленнее — на деле дольше
            return { kind: "full", hours: Math.max(0, ((sample.fullMah || 0) - q) / I) }
        return { kind: plugged ? "hold" : "left", hours: -1 }
    }
    // на панели — «ч:мм»: от часа — с шагом 5 мин (поменьше перерисовок панели), меньше часа — по минуте
    readonly property string clockText: {
        const f = forecast
        if (f.kind !== "left" || f.hours < 0)
            return ""
        const m = roundMinutes(f.hours)
        return Math.floor(m / 60) + ":" + (m % 60 < 10 ? "0" : "") + (m % 60)
    }
    function roundMinutes(h) {
        const m = Math.round(Math.min(h, 99.9) * 60)
        return m >= 60 ? Math.round(m / 5) * 5 : Math.max(1, m)
    }
    function duration(h) {   // «6 ч 40 мин», «35 мин»; больше двух суток — «3 сут»
        if (h >= 48)
            return Math.round(h / 24) + " сут"
        const m = roundMinutes(h)
        return m < 60 ? m + " мин" : Math.floor(m / 60) + " ч" + (m % 60 ? " " + (m % 60 < 10 ? "0" : "") + m % 60 + " мин" : "")
    }

    function take(s) {
        const now = Date.now()
        if (!known || now - takenAt > 120000) {
            // первое — среднее датчика (мгновенный ток может прийти всплеском); после смены зарядки — нынешний
            currentMa = reseed ? s.currentMa : s.currentAvgMa
            known = true
        } else {
            const dt = Math.min(30, (now - takenAt) / 1000)
            currentMa += (s.currentMa - currentMa) * (1 - Math.exp(-dt / 60))
        }
        reseed = false
        takenAt = now
        sample = s
    }
    onPluggedChanged: {
        known = false
        reseed = true
    }

    property Timer poll: Timer {
        interval: b.open ? 1000 : 15000
        running: b.live && (b.open || !b.plugged)
        repeat: true
        triggeredOnStart: true
        onTriggered: naomiPhone.battery.call("Sample", [], s => b.take(s))
    }

    function loadHistory() {
        const r = range
        naomiPhone.battery.call("History", r === "week" ? [7 * 86400, 168] : [86400, 96], h => {
            if (b.range === r)
                b.history = h
        })
    }
    onRangeChanged: if (open) loadHistory()
    property Timer historyTimer: Timer {
        interval: 60000
        running: b.live && b.open
        repeat: true
        triggeredOnStart: true
        onTriggered: b.loadHistory()
    }
}
