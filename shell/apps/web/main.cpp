// «Браузер» — Naomi Web с НаомиОС: отдельный процесс, движок Chromium из Qt WebEngine. Свою полосу вкладок
// не рисует: полосу и режим панели рисует стол оболочки, как bcdesk в НаомиОС (docs/WEB.md). Окружение задаёт
// оболочка (app_launcher): сокет Wayland, GPU через libhybris, масштаб стола 1,5.
#include "keyboard_context.h"
#include "unix_signals.h"
#include "web_bridge.h"
#include "web_filter.h"
#include "web_store.h"

#include <QDBusMessage>
#include <QDBusConnection>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QUrl>
#include <QtPlugin>

// модуль ввода «naomi» (QT_IM_MODULE ставит оболочка): клавиши зрителя стола приходят через него (D-Bus
// org.naomi.Input.p<pid>, keyboard_context.h) — любой раскладкой
Q_IMPORT_PLUGIN(KeyboardContextPlugin)

// Из <QtWebEngineQuick/qtwebenginequickglobal.h>. Заголовков WebEngine в среде сборки нет (пакета -dev не
// ставили), а из C++ нужна только эта функция: она экспортирована из libQt6WebEngineQuick.so.6 — той же
// сборки Qt 6.10.2, что в системе. Всё остальное — QML (модуль QtWebEngine).
namespace QtWebEngineQuick {
void initialize();
}

// Режим приложения: страница со ссылкой наружу (target=_blank) — браузеру стола новой вкладкой (оболочка:
// org.naomi.Web.Open, тот же путь, что у xdg-open Claude, docs/CLAUDE.md). Оболочки нет — ссылка не открывается.
class WebAppLinks : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void openOutside(const QUrl &url)
    {
        if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
            return;
        QDBusMessage call = QDBusMessage::createMethodCall("org.naomi.Web", "/web", "org.naomi.Web", "Open");
        call << url.toString();
        QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
    }
};

// Палитра окон, что рисует сам Qt WebEngine поверх страницы: alert / confirm / prompt сайта, вход на сайт, подсказка,
// выбор файла и цвета (QtWebEngine/ControlsDelegates: Qt Quick Controls и SystemPalette). Они были светлыми и
// выделялись на тёмном столе; здесь — цвета темы (Naomi.Ui Theme, DeskStyle): окно — surface, поле — уголь новой
// вкладки, выделение — янтарь .35. Стиль Controls — Fusion: «Basic» по умолчанию держит свою светлую палитру, и
// текст из этой (SystemPalette) выходил светлым по светлому (docs/WEB.md, «Тёмные сайты и полоса „Нить“»)
static QPalette darkPalette()
{
    const QColor bg("#0e0f0c"), surface("#161614"), raised("#1b1b18"), text("#ece9e3"), dim("#7d786e"),
        amber("#d8b25a");
    QPalette p;
    p.setColor(QPalette::Window, surface);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::AlternateBase, raised);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Button, raised);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, amber);
    p.setColor(QPalette::Light, QColor("#2e2c27"));
    p.setColor(QPalette::Midlight, QColor("#24231f"));
    p.setColor(QPalette::Mid, QColor("#2e2c27"));
    p.setColor(QPalette::Dark, QColor("#0a0a08"));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, QColor(216, 178, 90, 89));   // янтарь .35 — как выделение в строке адреса
    p.setColor(QPalette::HighlightedText, text);
    p.setColor(QPalette::Accent, amber);
    p.setColor(QPalette::Link, amber);
    p.setColor(QPalette::LinkVisited, amber);
    p.setColor(QPalette::ToolTipBase, surface);
    p.setColor(QPalette::ToolTipText, text);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, dim);
    return p;
}

// Режим приложения: naomi-web --app АДРЕС --name ИМЯ — одна страница во всё окно, без вкладок и связи со столом,
// свой профиль (~/.local/share/naomi/webapp/ИМЯ): веб-программа своим окном. Вход, клавиши, масштаб — как у браузера.
static int runApp(QGuiApplication &app, const QString &url, const QString &name)
{
    app.setApplicationName("naomi-webapp");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
    QGuiApplication::setPalette(darkPalette());
    app.setDesktopFileName(name);
    const QString data = QDir::homePath() + "/.local/share/naomi/webapp/" + name;
    const QString cache = QDir::homePath() + "/.cache/naomi/webapp/" + name;
    QDir().mkpath(data);
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloads);
    KeyboardContext::setRealKeys(true);
    QQmlApplicationEngine engine;
    engine.addImportPath(app.applicationDirPath() + "/../../qml");   // Naomi.Ui: крутилка ожидания
    WebAppLinks links;
    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty("appUrl", QUrl(url));
    ctx->setContextProperty("appName", name);
    ctx->setContextProperty("appDataDir", data);
    ctx->setContextProperty("appCacheDir", cache);
    ctx->setContextProperty("webDownloadDir", downloads);
    ctx->setContextProperty("appLinks", &links);
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QGuiApplication::quit);
    engine.load(QUrl::fromLocalFile(app.applicationDirPath() + "/qml/App.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *unixSignals = new UnixSignals(&app);
    QObject::connect(unixSignals, &UnixSignals::terminate, &app, &QGuiApplication::quit);
    // окно свернули — страница на паузе (App.qml: скрыта и Frozen), развернули — дальше (naomiApps.pause, как у игр)
    QObject *root = engine.rootObjects().constFirst();
    QObject::connect(unixSignals, &UnixSignals::pauseRequested, root, [root] { root->setProperty("paused", true); });
    QObject::connect(unixSignals, &UnixSignals::screenshot, root, [root] { root->setProperty("paused", false); });
    return app.exec();
}

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--enable-gpu-rasterization --enable-smooth-scrolling --ignore-gpu-blocklist");
    // до QGuiApplication: общий контекст GL движку и окну
    QtWebEngineQuick::initialize();
    // Qt в Ubuntu собран под настольный OpenGL, а GPU телефона (Android EGL) умеет только OpenGL ES.
    // Формат — после initialize(): она ставит свой, и без ES окно не создаёт поверхность EGL.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    QGuiApplication::setFont(QFont(QStringLiteral("Google Sans")));
    const QStringList args = app.arguments();
    const int appArg = args.indexOf("--app");
    if (appArg > 0 && appArg + 1 < args.size()) {
        const int nameArg = args.indexOf("--name");
        const QString name = nameArg > 0 && nameArg + 1 < args.size() ? args[nameArg + 1] : QStringLiteral("app");
        return runApp(app, args[appArg + 1], name);
    }
    app.setApplicationName("naomi-web");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");   // окна сайтов — по палитре darkPalette() (Main.qml)
    QGuiApplication::setPalette(darkPalette());
    app.setDesktopFileName("naomi-web");   // app_id окна: по нему стол узнаёт браузер

    // профиль: вход на сайтах, история, закладки — ~/.local/share/naomi/web; кэш — ~/.cache/naomi/web
    const QString data = QDir::homePath() + "/.local/share/naomi/web";
    const QString cache = QDir::homePath() + "/.cache/naomi/web";
    QDir().mkpath(data);
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloads);

    KeyboardContext::setRealKeys(true);   // сайтам — нажатия клавиш (keyboard_context.h)
    // реклама и слежка — мимо (web_filter.h); раньше движка QML: профиль страниц держит его до конца
    WebFilter filter(data + "/filters.txt");
    QQmlApplicationEngine engine;
    WebStore store(data, &engine);   // «Избранное», история, значки, сеанс
    WebBridge bridge;                // связь со столом
    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty("webDataDir", data);
    ctx->setContextProperty("webCacheDir", cache);
    ctx->setContextProperty("webDownloadDir", downloads);
    ctx->setContextProperty("webNewtab", QUrl::fromLocalFile(app.applicationDirPath() + "/newtab/newtab.html"));
    ctx->setContextProperty("webScripts", QUrl::fromLocalFile(app.applicationDirPath() + "/scripts").toString());
    ctx->setContextProperty("webStore", &store);
    ctx->setContextProperty("webBridge", &bridge);
    ctx->setContextProperty("webFilter", &filter);
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QGuiApplication::quit);
    engine.load(QUrl::fromLocalFile(app.applicationDirPath() + "/qml/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // оболочка закрывает программу SIGTERM: выйти мягко — движок допишет куки и историю
    auto *unixSignals = new UnixSignals(&app);
    QObject::connect(unixSignals, &UnixSignals::terminate, &app, &QGuiApplication::quit);
    return app.exec();
}

#include "main.moc"
