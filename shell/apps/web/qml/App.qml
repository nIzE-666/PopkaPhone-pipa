// Режим приложения Naomi Web (main.cpp, runApp): одна страница во всё окно — веб-программа со своим окном (её
// страница — на localhost). Пока страница не ответила (программа ещё запускается) — крутилка и новая попытка каждые
// 0,7 с; ответила — страница. Свой профиль: вход и настройки сайта не смешиваются с браузером. Ссылки наружу
// (target=_blank) — браузеру стола (appLinks).
// Окно свернули — пауза (paused: SIGUSR1, main.cpp): страница скрыта и Frozen — её скрипты и таймеры стоят: страница
// с непрерывной анимацией в свёрнутом окне ела ~1,5 ядра (замер 21 ноября 2026). Развернули (SIGUSR2) — с того же места.
import QtQuick
import QtQuick.Window
import QtWebEngine
import Naomi.Ui

Window {
    id: win
    width: 1280
    height: 800
    visible: true
    color: "#131416"
    title: view.ready && view.title ? view.title : appName

    // движок замораживает только невидимую страницу: сначала скрыть, потом Frozen; оттаять — наоборот (как Page.qml)
    property bool paused: false
    onPausedChanged: {
        if (paused) {
            view.visible = false
            Qt.callLater(() => view.lifecycleState = WebEngineView.LifecycleState.Frozen)
        } else {
            view.lifecycleState = WebEngineView.LifecycleState.Active
            view.visible = true
        }
    }

    WebEngineProfilePrototype {
        id: proto
        storageName: "naomi-webapp-" + appName
        persistentStoragePath: appDataDir
        cachePath: appCacheDir
        persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        settings.javascriptCanAccessClipboard: true
        settings.javascriptCanPaste: true
        property bool ready: false   // страница уже открывалась: дальше её ошибки — её дело (без шторки)
        opacity: ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        onLoadingChanged: (request) => {
            if (request.status === WebEngineView.LoadSucceededStatus)
                ready = true
            else if (request.status === WebEngineView.LoadFailedStatus && !ready)
                retry.start()
        }
        onNewWindowRequested: (request) => appLinks.openOutside(request.requestedUrl)
    }

    Timer {
        id: retry
        interval: 700
        onTriggered: view.url = appUrl
    }

    Spinner {
        anchors.centerIn: parent
        size: 28
        visible: !view.ready
    }

    Component.onCompleted: {
        // профиль — когда все его свойства заданы (иначе движок молча берёт «инкогнито», docs/WEB.md)
        const profile = proto.instance()
        profile.downloadPath = webDownloadDir
        const cleanUa = String(profile.httpUserAgent || "")
            .replace(/\s*QtWebEngine\/[0-9.]+/g, "")
            .replace(/Linux aarch64/g, "Linux x86_64")
            .replace(/\s{2,}/g, " ")
            .trim()
        profile.httpUserAgent = cleanUa
        if (profile.clientHints) {
            const m = cleanUa.match(/Chrome\/([0-9]+(?:\.[0-9]+)*)/)
            const ver = m ? m[1] : "134.0.0.0"
            profile.clientHints.arch = "x86"
            profile.clientHints.bitness = "64"
            profile.clientHints.platform = "Linux"
            profile.clientHints.isMobile = false
            profile.clientHints.fullVersion = ver
            profile.clientHints.fullVersionList = {
                "Chromium": ver,
                "Google Chrome": ver,
                "Not:A-Brand": "24.0.0.0"
            }
        }
        profile.downloadRequested.connect((download) => download.accept())
        view.profile = profile
        view.url = appUrl
    }
}
