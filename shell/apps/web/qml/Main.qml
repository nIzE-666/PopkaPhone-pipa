// «Браузер» — Naomi Web с НаомиОС на движке Qt WebEngine (docs/WEB.md). Окно — только страница открытой вкладки:
// полосу вкладок, строку адреса, «Избранное» и режим панели рисует стол оболочки (как bcdesk у Chromium с
// --kiosk). Со столом — webBridge: сюда — команды (activate, close, navigate…), туда — вкладки, закладки,
// загрузки (то, что в НаомиОС слало расширение bridge.js). Вкладки, сеанс, история, значки — здесь и в webStore.
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: win
    width: 1280
    height: 680
    visible: true
    color: "#0e0f0c"
    title: "Браузер"

    // Окна сайтов (alert, confirm, prompt, вход, подсказка, выбор файла) рисует Qt WebEngine — Qt Quick Controls поверх
    // страницы, и их цвета — палитра окна. Ей — тёмная палитра приложения (darkPalette в main.cpp): без этого Fusion
    // берёт свою светлую, а текст из SystemPalette — из нашей, светлым по светлому (docs/WEB.md, «Тёмные сайты…»)
    SystemPalette { id: sys }
    palette.window: sys.window
    palette.windowText: sys.windowText
    palette.base: sys.base
    palette.alternateBase: sys.alternateBase
    palette.text: sys.text
    palette.placeholderText: sys.placeholderText
    palette.button: sys.button
    palette.buttonText: sys.buttonText
    palette.light: sys.light
    palette.midlight: sys.midlight
    palette.mid: sys.mid
    palette.dark: sys.dark
    palette.shadow: sys.shadow
    palette.highlight: sys.highlight
    palette.highlightedText: sys.highlightedText
    palette.accent: sys.accent
    palette.toolTipBase: sys.window
    palette.toolTipText: sys.windowText

    WebEngineProfilePrototype {
        id: proto
        storageName: "naomi-web"
        persistentStoragePath: webDataDir
        cachePath: webCacheDir
        persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
    }
    property WebEngineProfile profile: null

    // ---------- вкладки ----------
    // [{id, view}] по порядку полосы; закреплённые — первыми (как держит их Chromium)
    property var tabs: []
    property int activeId: -1
    property int nextId: 1
    property var closedTabs: []   // закрытые — для Ctrl+Shift+T: [{url, index}], последняя в конце
    property int fullscreenId: -1
    // Стол никто не смотрит (docs/WEB.md, «Без зрителя»): зритель ушёл — видео и звук на паузу (pause); зрителя нет
    // 2 минуты или трансляцию выключили — ещё и страницы заморожены: сайты не выполняют скрипты и не тратят батарею;
    // столу — frozen, и он замораживает процессы браузера ядром. Зритель вернулся — оттаивают, каждая с того же места
    property bool frozen: false
    function pauseMedia() {
        for (const t of tabs)
            t.view.runJavaScript("document.querySelectorAll('video, audio').forEach(m => m.pause())")
    }
    function freeze(on) {
        if (on === frozen)
            return
        if (on) {
            pauseMedia()
            frozenLater.start()   // пауза дошла до страниц — тогда заморозить
        } else {
            frozenLater.stop()
            frozenSaid.stop()
            frozen = false
            console.info("web: страницы оттаяли")
        }
    }
    Timer {
        id: frozenLater
        interval: 300
        onTriggered: {
            win.frozen = true
            frozenSaid.start()   // Frozen дошёл до процессов страниц — тогда столу
            console.info("web: страницы заморожены:", win.tabs.length)
        }
    }
    Timer {
        id: frozenSaid
        interval: 200
        onTriggered: webBridge.send({ type: "frozen" })
    }

    function tab(id) { return tabs.find(t => t.id === id) }
    function indexOf(id) { return tabs.findIndex(t => t.id === id) }
    function activeTab() { return tab(activeId) }
    function isNewtab(url) { return String(url) === String(webNewtab) }

    // адрес из строки: как toUrl в bridge.js НаомиОС — адрес, localhost и IP — http, домен — https, иначе поиск Google
    function toUrl(text) {
        const s = String(text || "").trim()
        if (!s)
            return ""
        if (/^[a-z][a-z0-9+.-]*:\/\//i.test(s) || /^(about|data|file|view-source|chrome):/i.test(s))
            return s
        if (/^(localhost|\d{1,3}(\.\d{1,3}){3})(:\d+)?([/?#].*)?$/i.test(s))
            return "http://" + s
        if (!/\s/.test(s) && /^[^/?#]+\.[a-zЀ-ӿ]{2,}(:\d+)?([/?#].*)?$/i.test(s))
            return "https://" + s
        return "https://www.google.com/search?q=" + encodeURIComponent(s)
    }

    Item { id: pages; anchors.fill: parent }
    Component { id: pageComponent; Page { browser: win } }

    // новая вкладка: url — адрес или новая вкладка; after — рядом с какой (по умолчанию в конец);
    // background — не открывать; request — окно, которое просит страница (window.open, Ctrl-щелчок): берём его.
    // Без адреса — черновик (docs/WEB.md, «„+“ — экран, а не вкладка»): «Избранное» и «Часто» на месте новой вкладки;
    // вкладкой он станет, когда с него куда-то пойдут (keepDraft), а уйдут на другую вкладку — его нет (dropDraft).
    // Черновик уже открыт — второй не нужен
    function newTab(url, opts) {
        opts = opts || {}
        const draft = !url && !opts.request
        if (draft && activeTab() && activeTab().view.draft)
            return activeId
        const id = nextId++
        let view
        const fromSpare = draft && spare !== null
        if (fromSpare) {   // новая вкладка — из запаса: уже загружена; номер — ниже, после activate
            view = spare
            spare = null
            view.pinned = !!opts.pinned
        } else {
            view = pageComponent.createObject(pages, { tabId: id, pinned: !!opts.pinned })
            if (opts.request)
                opts.request.openIn(view)
            else
                view.url = url || webNewtab
        }
        if (draft) {
            view.draft = true
            draftFrom = activeId
        }
        const list = tabs.slice()
        const at = opts.after !== undefined ? indexOf(opts.after) : -1
        let i = at >= 0 ? at + 1 : list.length
        if (!opts.pinned)
            i = Math.max(i, list.filter(t => t.view.pinned).length)
        list.splice(i, 0, { id: id, view: view })
        tabs = list
        if (!opts.background || activeId < 0)
            activate(id)
        if (fromSpare)
            view.tabId = id   // номер — после activate: activeId уже он, страница показывается один раз
        changed()
        return id
    }

    // Новая вкладка про запас (docs/WEB.md, «Новая вкладка — сразу»): «+» открывал вкладку ~320 мс — QML страницы
    // ~90 мс, вкладка Chromium ~66, новый процесс страницы ~140 (замер 17 ноября 2026). Поэтому одна новая вкладка
    // собрана заранее: Page с newtab.html в своём процессе, скрыта (tabId −2: в полосе и сеансе её нет). «+», Ctrl+T,
    // закрыли последнюю — newTab берёт её черновиком; черновик стал вкладкой — следующая через 2 с, бросили — он
    // снова запас. Держать её видимой для движка (кадр заранее) — пробовали: не быстрее.
    property Page spare: null
    Timer { id: spareLater; interval: 2000; onTriggered: win.makeSpare() }
    function makeSpare() {
        if (spare || !profile)
            return
        spare = pageComponent.createObject(pages, { tabId: -2 })
        spare.url = webNewtab
    }

    // черновик: с какой вкладки пришли (закрыли черновик — обратно туда); стал вкладкой; бросили
    property int draftFrom: -1
    function keepDraft(view) {
        view.draft = false
        spareLater.restart()
        changed()
    }
    function dropDraft(t) {
        tabs = tabs.filter(o => o !== t)
        if (spare) {
            t.view.destroy()
        } else {   // по-прежнему newtab.html — пусть ждёт следующего «+»
            t.view.draft = false
            t.view.tabId = -2
            spare = t.view
        }
        changed()
    }

    function activate(id) {
        if (!tab(id))
            return
        const was = activeTab()
        activeId = id
        if (was && was.id !== id && was.view.draft)
            dropDraft(was)
        const t = activeTab()
        t.view.forceActiveFocus()
        // «Часто» — заново, как visibilitychange в newtab.js; но после показа: подсчёт по истории — на главном потоке,
        // и вкладка ждала его ~100 мс (docs/WEB.md, «Новая вкладка — сразу»)
        if (isNewtab(t.view.url)) {
            const v = t.view
            Qt.callLater(() => { if (v && win.isNewtab(v.url)) v.fillNewtab() })
        }
        changed()
    }

    // последняя вкладка не закрывается: сначала черновик новой (браузер закрывают кнопкой окна) — как в НаомиОС.
    // Закрыть черновик — вернуться, откуда пришли; один черновик и остался — он и так «новая вкладка»
    function closeTab(id) {
        const i = indexOf(id)
        if (i < 0)
            return
        if (tabs[i].view.draft) {
            if (tabs.length > 1)
                activate(tab(draftFrom) ? draftFrom : tabs[i > 0 ? i - 1 : 1].id)
            return
        }
        if (tabs.length === 1)
            newTab("")
        const t = tabs[i]
        if (!isNewtab(t.view.url))
            closedTabs = closedTabs.concat([{ url: String(t.view.url), index: i }]).slice(-20)
        const list = tabs.slice()
        list.splice(i, 1)
        tabs = list
        if (activeId === id)
            activate(list[Math.min(i, list.length - 1)].id)
        if (fullscreenId === id)
            setFullscreen(-1)
        t.view.destroy()
        changed()
    }

    function restoreClosed() {
        if (!closedTabs.length)
            return
        const c = closedTabs[closedTabs.length - 1]
        closedTabs = closedTabs.slice(0, -1)
        const id = newTab(c.url)
        moveTo(id, Math.min(c.index, tabs.length - 1))
    }

    function moveTo(id, index) {
        const i = indexOf(id)
        if (i < 0)
            return
        const list = tabs.slice()
        const t = list.splice(i, 1)[0]
        list.splice(index, 0, t)
        tabs = list
        changed()
    }

    function pin(id, on) {
        const t = tab(id)
        if (!t || t.view.pinned === on)
            return
        t.view.pinned = on
        const pinnedCount = tabs.filter(o => o.view.pinned && o.id !== id).length
        moveTo(id, pinnedCount)   // закреплённая — в конец закреплённых, откреплённая — сразу за ними
    }

    function cycle(step) {
        if (tabs.length < 2)
            return
        const i = indexOf(activeId)
        activate(tabs[(i + step + tabs.length) % tabs.length].id)
    }

    function setFullscreen(id) {
        fullscreenId = id
        webBridge.fullscreen = id >= 0
        webBridge.send({ type: "fullscreen", on: id >= 0 })
    }

    // ---------- стол: что показать ----------
    // имя и адрес — как shownTitle/shownUrl в bridge.js: новая вкладка — пустой адрес; уходит на сайт — его адрес и
    // хост без www, пока сайт не ответил
    function host(url) {
        const m = String(url).match(/^[a-z][a-z0-9+.-]*:\/\/([^/?#:]+)/i)
        return m ? m[1].replace(/^www\./, "") : String(url)
    }
    function tabInfo(t) {
        const v = t.view
        const going = v.pendingUrl !== "" && v.pendingUrl !== String(v.url)
        const url = going ? v.pendingUrl : isNewtab(v.url) ? "" : String(v.url)
        const title = going ? host(v.pendingUrl) : isNewtab(v.url) ? "Новая вкладка" : (v.title || url || "Новая вкладка")
        return { id: t.id, pinned: v.pinned, loading: v.loading, title: title, url: url,
                 icon: url ? webStore.iconUrl(url) : "", draft: v.draft }
    }
    function sendTabs() {
        webBridge.send({ type: "tabs", active: activeId, tabs: tabs.map(tabInfo) })
    }
    function sendBookmarks() {
        webBridge.send({ type: "bookmarks", items: webStore.bookmarks })
    }
    // любое изменение вкладок — столу через 30 мс (как queueState в bridge.js), сеанс — через секунду
    function changed() {
        tabsLater.restart()
        sessionLater.restart()
    }
    Timer { id: tabsLater; interval: 30; onTriggered: win.sendTabs() }
    Timer {   // пока что-то грузится — сверка раз в секунду (переход мог кончиться без сигнала)
        interval: 1000
        repeat: true
        running: win.tabs.some(t => t.view.loading)
        onTriggered: win.sendTabs()
    }

    // ---------- сеанс: вкладки прошлого раза ----------
    // пустые (черновик, вкладка, вернувшаяся «Назад» на новую) — не вкладки: не пишутся и не поднимаются; открыт
    // черновик — открытой запомнится та, с которой пришли
    Timer { id: sessionLater; interval: 1000; onTriggered: win.saveSession() }
    function saveSession() {
        const kept = tabs.filter(t => !t.view.draft && !isNewtab(t.view.url))
        const a = activeTab()
        const shown = a && a.view.draft ? draftFrom : activeId
        webStore.saveSession({ tabs: kept.map(t => ({ url: String(t.view.url), pinned: t.view.pinned })),
                               active: Math.max(0, kept.findIndex(t => t.id === shown)) })
    }
    function restoreSession() {
        const s = webStore.loadSession()
        const all = s.tabs || []
        const list = all.filter(t => t.url)
        for (const t of list)
            newTab(t.url, { pinned: t.pinned, background: true })
        if (!tabs.length)
            newTab("")
        const at = all.slice(0, s.active || 0).filter(t => t.url).length   // сеанс прошлых версий — с пустыми
        activate(tabs[Math.min(at, tabs.length - 1)].id)
    }

    // ---------- загрузки ----------
    property var downloads: []   // [WebEngineDownloadRequest], новые — в начале, не больше 3 (как в НаомиОС)
    function downloadInfo(d) {
        const state = d.state === WebEngineDownloadRequest.DownloadCompleted ? "completed"
                    : d.state === WebEngineDownloadRequest.DownloadInProgress
                      || d.state === WebEngineDownloadRequest.DownloadRequested ? "progressing" : "interrupted"
        return { id: d.id, name: d.downloadFileName || "файл", received: d.receivedBytes, total: d.totalBytes,
                 state: state, path: d.downloadDirectory + "/" + d.downloadFileName }
    }
    function sendDownloads() {
        webBridge.send({ type: "downloads", items: downloads.map(downloadInfo) })
    }
    Timer {   // пока качается — раз в 0,5 с
        interval: 500
        repeat: true
        running: win.downloads.some(d => d.state === WebEngineDownloadRequest.DownloadInProgress)
        onTriggered: win.sendDownloads()
    }
    Connections {
        target: win.profile
        function onDownloadRequested(download) {
            download.accept()
            win.downloads = [download].concat(win.downloads).slice(0, 3)
            download.stateChanged.connect(win.sendDownloads)
            win.sendDownloads()
        }
    }

    // ---------- стол ----------
    Connections {
        target: webBridge
        function onLinked() {
            win.sendTabs()
            win.sendBookmarks()
            win.sendDownloads()
            if (win.fullscreenId >= 0)
                webBridge.send({ type: "fullscreen", on: true })
        }
        function onCommand(m) {
            const t = m.id !== undefined ? win.tab(m.id) : win.activeTab()
            switch (m.type) {
            case "new-tab": win.newTab(win.toUrl(m.text)); break
            case "activate": win.activate(m.id); break
            case "close": win.closeTab(m.id); break
            case "navigate": { const url = win.toUrl(m.text); if (t && url) t.view.url = url; break }
            case "pin": win.pin(m.id, !!m.pinned); break
            case "reload": if (t) t.view.reload(); break
            case "back": if (t) t.view.goBack(); break
            case "forward": if (t) t.view.goForward(); break
            case "toggle-dark": if (t) t.view.darkAllowed = !t.view.darkAllowed; break
            case "find": if (t) t.view.findText(m.text || ""); break
            case "duplicate": if (t) win.newTab(String(t.view.url), { after: t.id }); break
            case "restore-closed": win.restoreClosed(); break
            case "cycle": win.cycle(m.step); break
            case "select": {
                const i = m.index < 0 ? win.tabs.length - 1 : m.index
                if (i < win.tabs.length) win.activate(win.tabs[i].id)
                break
            }
            case "dismiss-download":
                win.downloads = win.downloads.filter(d => d.id !== m.id)
                win.sendDownloads()
                break
            case "bookmark": webStore.setBookmark(m.url, m.title || "", !!m.on); break
            case "bookmark-rename": webStore.renameBookmark(m.id, m.title); break
            case "bookmark-delete": webStore.removeBookmark(m.id, m.url || ""); break
            case "open": {
                const a = win.activeTab()
                if (m.newTab || !a) win.newTab(m.url)
                else a.view.url = m.url
                break
            }
            case "focus": if (win.activeTab()) win.activeTab().view.forceActiveFocus(); break
            case "freeze": win.freeze(!!m.on); break
            case "pause": win.pauseMedia(); break
            case "quit": win.saveSession(); Qt.quit(); break
            }
        }
        function onShortcut(action, index) {
            const t = win.activeTab()
            switch (action) {
            case "field": webBridge.send({ type: "field" }); break
            case "new-tab": win.newTab(""); webBridge.send({ type: "field", text: "" }); break
            case "restore-closed": win.restoreClosed(); break
            case "close": if (t) win.closeTab(t.id); break
            case "next": win.cycle(1); break
            case "previous": win.cycle(-1); break
            case "select": {
                const i = index < 0 ? win.tabs.length - 1 : index
                if (i < win.tabs.length) win.activate(win.tabs[i].id)
                break
            }
            case "reload": if (t) t.view.reload(); break
            case "back": if (t) t.view.goBack(); break
            case "forward": if (t) t.view.goForward(); break
            case "zoom-in": if (t) t.view.zoomStep(1); break
            case "zoom-out": if (t) t.view.zoomStep(-1); break
            case "zoom-reset": if (t) t.view.zoomFactor = 1; break
            case "exit-fullscreen": if (t) t.view.triggerWebAction(WebEngineView.ExitFullScreen); break
            }
        }
    }
    Connections {
        target: webStore
        function onBookmarksChanged() {
            win.sendBookmarks()
            for (const t of win.tabs)
                if (t.view.newtab)
                    t.view.fillNewtab()
        }
        function onIconSaved(site, url) { win.changed() }
    }

    function cleanUserAgent(ua) {
        return String(ua || "")
            .replace(/\s*QtWebEngine\/[0-9.]+/g, "")
            .replace(/Linux aarch64/g, "Linux x86_64")
            .replace(/\s{2,}/g, " ")
            .trim()
    }
    function applyCleanProfileIdentity(p) {
        const cleanUa = cleanUserAgent(p.httpUserAgent)
        p.httpUserAgent = cleanUa
        if (p.clientHints) {
            const m = cleanUa.match(/Chrome\/([0-9]+(?:\.[0-9]+)*)/)
            const ver = m ? m[1] : "134.0.0.0"
            p.clientHints.arch = "x86"
            p.clientHints.bitness = "64"
            p.clientHints.platform = "Linux"
            p.clientHints.isMobile = false
            p.clientHints.fullVersion = ver
            p.clientHints.fullVersionList = {
                "Chromium": ver,
                "Google Chrome": ver,
                "Not:A-Brand": "24.0.0.0"
            }
        }
    }

    Component.onCompleted: {
        // профиль — когда все его свойства уже заданы: иначе движок берёт профиль «инкогнито» (проверено).
        // User-Agent — как у Chrome той же версии, без «QtWebEngine/…»: с ним часть сайтов считает браузер странным.
        profile = proto.instance()
        webFilter.attach(profile)   // реклама и слежка — мимо (web_filter.h)
        // Google Sign-In (accounts.google.com): align navigator identity with auth headers
        const gauth = WebEngine.script()
        gauth.name = "naomi-google-auth"
        gauth.sourceUrl = webScripts + "/google-auth.js"
        gauth.injectionPoint = WebEngineScript.DocumentCreation
        gauth.worldId = WebEngineScript.MainWorld
        gauth.runsOnSubFrames = true
        profile.userScripts.insert(gauth)
        // YouTube — в H.264, а не в AV1: видео процессором, AV1 ему не по силам (scripts/youtube-h264.js)
        const h264 = WebEngine.script()
        h264.name = "naomi-youtube-h264"
        h264.sourceUrl = webScripts + "/youtube-h264.js"
        h264.injectionPoint = WebEngineScript.DocumentCreation
        h264.worldId = WebEngineScript.MainWorld
        h264.runsOnSubFrames = true
        profile.userScripts.insert(h264)
        // сайт нарисовал первое содержимое — снять шторку (scripts/painted.js, Page.qml)
        const painted = WebEngine.script()
        painted.name = "naomi-painted"
        painted.sourceUrl = webScripts + "/painted.js"
        painted.injectionPoint = WebEngineScript.DocumentCreation
        painted.worldId = WebEngineScript.ApplicationWorld
        profile.userScripts.insert(painted)
        // полоса прокрутки страниц — «Нить», а не белая Chromium (scripts/scrollbar.js); у сайта своя — своя
        const scrollbar = WebEngine.script()
        scrollbar.name = "naomi-scrollbar"
        scrollbar.sourceUrl = webScripts + "/scrollbar.js"
        scrollbar.injectionPoint = WebEngineScript.DocumentCreation
        scrollbar.worldId = WebEngineScript.ApplicationWorld
        scrollbar.runsOnSubFrames = true
        profile.userScripts.insert(scrollbar)
        profile.downloadPath = webDownloadDir
        applyCleanProfileIdentity(profile)
        restoreSession()
        spareLater.start()   // запас новой вкладки — когда вкладки прошлого раза уже грузятся
    }
}
