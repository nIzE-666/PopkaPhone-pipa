// Вкладка «Браузера»: страница (WebEngineView) во всё окно; видна только открытая — остальные Chromium считает
// скрытыми и придерживает. Что делает страница: окно просит (window.open, Ctrl-щелчок, вход «через Google») — новой
// вкладкой рядом; правый щелчок — меню страницы НаомиОС на столе (сайт показал своё — нет, как в НаомиОС);
// видео на весь экран — столу; значок сайта — в профиль (стол рисует его на полосе); загрузилась — в историю.
import QtQuick
import QtWebEngine

WebEngineView {
    id: view
    property var browser                 // Main.qml
    property int tabId
    property bool pinned: false
    property string pendingUrl: ""       // куда уходит, пока сайт не ответил (как pendingUrl у Chromium)
    readonly property bool newtab: browser.isNewtab(url)
    property bool draft: false           // черновик новой вкладки, пока с него никуда не пошли (Main.qml, newTab)

    anchors.fill: parent
    // Фон под страницей. По умолчанию белый — и первый кадр новой вкладки был белым (1–3 кадра между тёмным окном и
    // тёмной новой вкладкой, замер 17 ноября 2026). Теперь у всех — фон новой вкладки (--bg в newtab.html): сайты
    // тёмные (forceDarkMode ниже), и страница без своего фона — светлым текстом на нём
    backgroundColor: "#0e0f0c"
    // Сайты — тёмные, как весь стол (просьба пользователя 21 ноября 2026: «везде, где белый фон, — как фон новой
    // вкладки»): светлую страницу перекрашивает сам движок (тёмный режим Chromium), картинки и видео не трогает.
    // Новой вкладке не нужно — она тёмная своя (docs/WEB.md, «Тёмные сайты и полоса „Нить“»)
    property bool darkAllowed: true
    settings.forceDarkMode: !newtab && darkAllowed
    // Шторка цвета новой вкладки, пока сайт ничего не нарисовал: этот белый фон пустой страницы и был белым экраном
    // на холодном запуске YouTube (docs/WEB.md, «Без белого экрана на холодную»). Сайт нарисовал первое содержимое
    // (scripts/painted.js) — шторки больше нет, вид дальше живёт без неё. Знака нет — снять, когда загрузка кончилась
    // (через 0,3 с: дать дорисоваться) или страница 10 с на экране: иначе шторка могла бы спрятать сайт навсегда
    property bool painted: false
    Rectangle {
        anchors.fill: parent
        z: 1   // над содержимым движка (его вид — тоже ребёнок WebEngineView)
        color: "#0e0f0c"
        visible: !view.painted && !view.newtab && String(view.url) !== ""
    }
    Timer { id: paintedLater; interval: 300; onTriggered: view.painted = true }
    Timer { interval: 10000; running: view.shown && !view.painted; onTriggered: view.painted = true }
    // заморожена — скрыта (движок замораживает только невидимую), потом — Frozen; оттаяла — наоборот
    readonly property bool frozen: browser.frozen
    readonly property bool shown: tabId === browser.activeId
    visible: shown && !frozen
    onFrozenChanged: Qt.callLater(applyLifecycle)
    // Фоновая вкладка засыпает (docs/WEB.md, «Фоновые вкладки»): скрыта 2 минуты, звука нет и движок не против
    // (recommendedState — не Active: нет загрузки, отладчика и т. п.) — Frozen: скрипты и таймеры сайта стоят (такая
    // ела ~1/6 ядра), место на странице и вход целы; открыли — сразу Active. Движок не согласен — спросить снова позже
    property bool sleeping: false
    function applyLifecycle() {
        lifecycleState = frozen || sleeping ? WebEngineView.LifecycleState.Frozen : WebEngineView.LifecycleState.Active
    }
    onShownChanged: {
        if (shown) {
            sleepLater.stop()
            if (sleeping) {
                sleeping = false
                applyLifecycle()
            }
        } else {
            sleepLater.restart()
        }
    }
    onTabIdChanged: if (tabId < 0) sleepLater.stop()   // брошенный черновик снова запас — ему спать незачем
    Timer {
        id: sleepLater
        interval: 120000
        onTriggered: {
            if (view.shown || view.sleeping)
                return
            if (view.recentlyAudible || view.recommendedState === WebEngineView.LifecycleState.Active) {
                restart()
                return
            }
            view.sleeping = true
            view.applyLifecycle()
            console.info("web: фоновая вкладка уснула:", view.title)
        }
    }
    profile: browser.profile
    settings.fullScreenSupportEnabled: true
    settings.dnsPrefetchEnabled: true   // адреса ссылок — заранее (в Qt WebEngine по умолчанию выключено)
    settings.localContentCanAccessFileUrls: true   // значки «Избранного» на новой вкладке — файлы профиля
    // Новая вкладка — файл (newtab.html), и движок не пускал её на сайты: щелчок по плитке давал «Your Internet access
    // is blocked» (проверено 20 ноября 2026; фоновые открытия шли мимо — их открывает программа). Разрешено только ей:
    // свой файл со своими данными; прочим локальным страницам — по-прежнему нет
    settings.localContentCanAccessRemoteUrls: newtab
    settings.javascriptCanAccessClipboard: true
    settings.javascriptCanPaste: true

    // новая вкладка: «Избранное» и «Часто» (newtab.js — naomiNewtab)
    function fillNewtab() {
        const data = { favorites: webStore.bookmarks, frequent: webStore.frequent(30, 8) }
        runJavaScript("window.naomiNewtab && naomiNewtab(" + JSON.stringify(data) + ")")
    }

    // шаги масштаба — как у Chrome
    readonly property var zoomSteps: [0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4, 5]
    function zoomStep(dir) {
        const i = zoomSteps.findIndex(z => z >= zoomFactor - 0.001)
        const at = i < 0 ? zoomSteps.length - 1 : i
        zoomFactor = zoomSteps[Math.max(0, Math.min(zoomSteps.length - 1,
                                                    dir > 0 ? at + (zoomSteps[at] > zoomFactor + 0.001 ? 0 : 1) : at - 1))]
    }

    onLoadingChanged: (info) => {
        if (info.status === WebEngineView.LoadStartedStatus) {
            pendingUrl = String(info.url)
            if (draft && !browser.isNewtab(info.url))   // с черновика пошли на сайт — теперь это вкладка
                browser.keepDraft(view)
        } else {
            pendingUrl = ""
            if (!painted)
                paintedLater.start()
            if (info.status === WebEngineView.LoadSucceededStatus) {
                if (newtab)
                    fillNewtab()
                else
                    webStore.visit(String(url), title)
            }
        }
        browser.changed()
    }
    onTitleChanged: {
        webStore.retitle(String(url), title)
        browser.changed()
    }
    onUrlChanged: browser.changed()
    onIconChanged: if (String(icon) !== "") webStore.saveIcon(String(url), icon)

    onNewWindowRequested: (request) => {
        const background = request.destination === WebEngineNewWindowRequest.InNewBackgroundTab
        browser.newTab("", { request: request, after: tabId, background: background })
    }
    onWindowCloseRequested: browser.closeTab(tabId)
    onContextMenuRequested: (request) => {
        request.accepted = true
        webBridge.send({ type: "contextmenu", tab: tabId })
    }
    onFullScreenRequested: (request) => {
        request.accept()
        browser.setFullscreen(request.toggleOn ? tabId : -1)
    }
    // с новой вкладки (newtab.js): «открыть в новой рядом» и корзина у строки «Часто» — забыть сайт и показать
    // «Часто» заново (снизу встаёт следующий сайт)
    onJavaScriptConsoleMessage: (level, message, line, source) => {
        if (message === "naomi:painted") {
            painted = true
            return
        }
        if (!newtab)
            return
        if (message.startsWith("naomi:open-bg ")) {
            browser.newTab(message.slice(14), { after: tabId, background: true })
        } else if (message.startsWith("naomi:forget ")) {
            webStore.forgetSite(message.slice(13))
            fillNewtab()
        }
    }
    // процесс страницы упал — грузим заново (у Chrome — «Опаньки», здесь просто снова)
    onRenderProcessTerminated: (status, code) => {
        if (status !== WebEngineView.NormalTerminationStatus)
            reloadLater.start()
    }
    Timer { id: reloadLater; interval: 500; onTriggered: view.reload() }
}
