// НаомиФон — оболочка и Wayland-композитор: экран блокировки, главный экран, окна программ, шторка,
// клавиатура. QML читается с диска рядом с программой (qml/, общий модуль Naomi.Ui —
// ../qml): правка интерфейса не требует сборки C++.
// В QML: naomiDisplay (кадры, частота), naomiPower (экран, AOD, яркость), naomiPhone (железо: служба naomid —
// батарея, Wi-Fi, сотовая сеть, звук, Bluetooth), naomiSystem (память, процессор, адреса, трафик — «Настройки»),
// naomiApps (запуск программ), naomiAndroid (Android-приложения — android_apps.h), naomiGames (игры стола — game_library.h), naomiPadKeys
// (кнопки геймпада клавишами — pad_keys.h), naomiKeyboard (ввод текста),
// naomiProject (проект студии NaomiPhone: экраны блокировки и главный — project_file.h),
// naomiStream (трансляция экрана на Mac: зритель, к/с — stream_service.h), naomiFiles и тип Naomi.Files/FilesModel
// («Файлы» рабочего стола — shell/files),
// naomiDensity (плотность экрана), naomiTrace (при NAOMI_TRACE, иначе null); столу — naomiDesk (масштаб, покой),
// naomiShot (скриншоты стола — desk_shot.h), naomiPreview с типами Naomi.Preview («Просмотр» — shell/preview) и
// naomiForce («закрыть принудительно» меню N стола — force_quit.h).
#include "android_apps.h"
#include "android_move.h"
#include "android_surface.h"
#include "web_surface.h"
#include "app_launcher.h"
#include "force_quit.h"
#include "code_pin.h"
#include "desk_probe.h"
#include "desk_shot.h"
#include "desktop_screen.h"
#include "device_profile.h"
#include "game_library.h"
#include "pad_keys.h"
#include "sound_equalizer.h"
#include "system_monitor.h"
#include "dev_screenshot.h"
#include "dev_trace.h"
#include "display_stats.h"
#include "files_model.h"
#include "files_service.h"
#include "keyboard_context.h"
#include "phone_client.h"
#include "preview_service.h"
#include "preview_video.h"
#include "power.h"
#include "haptics.h"
#include "project_file.h"
#include "spell.h"
#include "stream_service.h"
#include "system_stats.h"
#include "system_tools.h"
#include "unix_signals.h"
#include "video_plane.h"
#include "term_link.h"
#include "term_view.h"
#include "shell_compositor.h"
#include "web_link.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QFont>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QWaylandCompositor>
#include <QWaylandQuickOutput>
#include <QWaylandViewporter>
#include <QtPlugin>

#include <malloc.h>
#include <memory>
#include <wayland-server-core.h>

Q_IMPORT_PLUGIN(KeyboardContextPlugin)

int main(int argc, char **argv)
{
    // Память — обратно системе. Большие куски (от 1 МБ: фото и кадры «Просмотра», снимки стола) — отдельными
    // отображениями, которые free возвращает сразу: по умолчанию glibc после первого же такого куска поднимает порог
    // до 32 МБ и дальше берёт их из общей кучи, а куча с дырами обратно не отдаётся — оболочка за десять прогонов
    // стола выросла с 228 до 666 МБ, хотя после malloc_trim оставалось ~305 (рентген 19 ноября 2026, docs/XRAY.md).
    // Арен — не больше четырёх (по умолчанию — 8 на ядро: у каждого потока своя куча со своими дырами)
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
    mallopt(M_TRIM_THRESHOLD, 8 << 20);
    mallopt(M_ARENA_MAX, 4);
    qputenv("QT_IM_MODULE", "naomi");   // своя клавиатура (keyboard_context); программам не передаётся
    // Ширина кадра (docs/DISPLAY.md, «Кадр меньше панели»): выбор из «Настроек» (display/render), окружение
    // NAOMI_RENDER — главнее. Драйвер экрана читает её при старте: смена — перезапуском оболочки.
    if (!qEnvironmentVariableIsSet("NAOMI_RENDER"))
        qputenv("NAOMI_RENDER", QByteArray::number(DisplayStats::savedRenderWidth()));
    ProbedApp app(argc, argv);   // долгие события потока интерфейса — в трассу стола (desk_probe.h)
    app.setApplicationName("naomi-shell");
    pinCodeStart();   // код оболочки — не вытеснять на диск (code_pin.h)
    QGuiApplication::setFont(QFont(QStringLiteral("Google Sans")));   // где шрифт не задан (Theme.sans); иначе — DejaVu Sans

    const QString waylandSocket = "wayland-naomi";
    const auto &profile = DeviceProfile::current();
    // плотность — от драйвера экрана (2,75 на полной ширине, меньше — в той же доле): у программ та же
    const double density = app.primaryScreen()->devicePixelRatio();

    // объекты для QML живут дольше движка: при его уничтожении QML ещё обращается к ним
    DisplayStats stats;
    PhoneClient phone;
    SystemStats system;
    SystemTools systemTools(profile.tablet);
    Power power;
    Haptics haptics;   // вибромотор: отклик клавиатуры (haptics.h)
    auto *keyboard = KeyboardContext::instance();
    if (profile.tablet && keyboard) {
        keyboard->setManualOnly(true);
        keyboard->setDismissOnFocusLoss(true);
        QObject::connect(&power, &Power::displayOnChanged, keyboard, [&power, keyboard] {
            if (!power.displayOn())
                keyboard->hideInputPanel();
        });
    }
    AppLauncher apps(waylandSocket, density);
    AndroidApps android(waylandSocket);   // Android-приложения (Waydroid, docs/ANDROID.md)
    GameLibrary games;                    // игры стола (docs/GAMES.md)
    PadKeys padKeys(&games);              // кнопки геймпада клавишами (docs/GAMES.md, «Кнопки геймпада»)
    ProjectFile project;
    StreamService streamService;   // трансляция экрана на Mac (docs/STREAM.md)
    if (profile.tablet && keyboard) {
        QPointer<StreamService> stream(&streamService);
        keyboard->setKeyFallback([stream](bool down, int qtKey, int modifiers, const QString &text) {
            if (stream)
                stream->injectKey(down, qtKey, modifiers, text);
        });
        QObject::connect(keyboard, &KeyboardContext::layoutChanged, &streamService, [keyboard, &streamService] {
            if (!streamService.layout().isEmpty())
                streamService.setLayout(keyboard->layout());
        });
        QObject::connect(&streamService, &StreamService::layoutChanged, keyboard, [keyboard, &streamService] {
            if (!streamService.layout().isEmpty())
                keyboard->setLayout(streamService.layout());
        });
    }
    FilesService files;            // «Файлы» рабочего стола (docs/FILES.md)
    PreviewService preview;        // «Просмотр» стола: фото и видео (docs/DESKTOP.md, «Просмотр»)
    WebLink web;                   // связь стола с «Браузером» (docs/WEB.md)
    TermLink term;                 // «Терминал» стола: вкладки и служба naomi-term (docs/TERMINAL.md)
    ForceQuit force(&apps, &term); // «закрыть принудительно» меню N стола (docs/DESKTOP.md)
    qmlRegisterType<FilesModel>("Naomi.Files", 1, 0, "FilesModel");
    qmlRegisterType<SoundEqualizer>("Naomi.Desk", 1, 0, "SoundEqualizer");   // эквалайзер окна звука стола
    qmlRegisterType<MonitorGraph>("Naomi.Desk", 1, 0, "MonitorGraph");       // график монитора системы
    qmlRegisterType<AndroidMove>("Naomi.Desk", 1, 0, "AndroidMove");         // окно Android тащит стол
    qmlRegisterType<AndroidSurface>("Naomi.Desk", 1, 0, "AndroidSurface");   // поверхность окна Android
    qmlRegisterType<WebSurface>("Naomi.Desk", 1, 0, "WebSurface");           // страница браузера по контуру окна
    qmlRegisterType<TermView>("Naomi.Desk", 1, 0, "TermView");               // текст вкладки «Терминала»
    qmlRegisterType<ShellCompositor>("Naomi.Shell", 1, 0, "ShellCompositor");   // корень Main.qml: общий буфер обмена
    qmlRegisterType<VideoPlane>("Naomi.Preview", 1, 0, "VideoPlane");        // кадр видео «Просмотра» — текстурами
    qmlRegisterUncreatableType<PreviewVideo>("Naomi.Preview", 1, 0, "PreviewVideo", QStringLiteral("naomiPreview.video"));
    std::unique_ptr<DevTrace> trace(qEnvironmentVariableIsSet("NAOMI_TRACE") ? new DevTrace : nullptr);
    // данные службы — пока горит экран или смотрят трансляцию: стол у приставки живёт, пока телефон лежит в AOD, и
    // звук, Bluetooth, Wi-Fi нужны ему свежими (без этого окно звука считало шаг от громкости, устаревшей на минуты)
    auto onScreen = [&] {
        stats.setActive(power.displayOn());
        phone.setActive(power.displayOn() || streamService.connected(), power.dozing());
        power.setDesk(streamService.desktop());
    };
    QObject::connect(&power, &Power::displayOnChanged, &app, onScreen);
    QObject::connect(&power, &Power::dozingChanged, &app, onScreen);
    QObject::connect(&streamService, &StreamService::changed, &app, onScreen);

    Spell spell;   // исправление опечаток клавиатуры (spell.h); живёт дольше движка QML
    QQmlApplicationEngine engine;
    phone.setEngine(&engine);
    engine.addImportPath(app.applicationDirPath() + "/../qml");
    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty("naomiDisplay", &stats);
    ctx->setContextProperty("naomiPower", &power);
    ctx->setContextProperty("naomiHaptics", &haptics);
    ctx->setContextProperty("naomiPhone", &phone);
    ctx->setContextProperty("naomiSystem", &system);
    ctx->setContextProperty("naomiSystemTools", &systemTools);
    ctx->setContextProperty("naomiApps", &apps);
    ctx->setContextProperty("naomiAndroid", &android);
    ctx->setContextProperty("naomiGames", &games);
    ctx->setContextProperty("naomiPadKeys", &padKeys);
    ctx->setContextProperty("naomiProject", &project);
    ctx->setContextProperty("naomiKeyboard", keyboard);
    ctx->setContextProperty("naomiSpell", &spell);
    ctx->setContextProperty("naomiStream", &streamService);
    ctx->setContextProperty("naomiFiles", &files);
    ctx->setContextProperty("naomiPreview", &preview);
    ctx->setContextProperty("naomiWeb", &web);
    ctx->setContextProperty("naomiTerm", &term);
    ctx->setContextProperty("naomiForce", &force);
    ctx->setContextProperty("naomiDensity", density);
    ctx->setContextProperty("naomiTablet", profile.tablet);
    ctx->setContextProperty("naomiWaylandSocket", waylandSocket);
    ctx->setContextProperty("naomiTrace", trace.get());
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QGuiApplication::quit);

    const QString qml = qEnvironmentVariable("NAOMI_QML", app.applicationDirPath() + "/qml/Main.qml");
    engine.load(QUrl::fromLocalFile(qml));
    auto *compositor = qobject_cast<QWaylandCompositor *>(engine.rootObjects().value(0));
    auto *output = compositor ? qobject_cast<QWaylandQuickOutput *>(compositor->property("displayOutput").value<QObject *>())
                              : nullptr;
    auto *window = output ? qobject_cast<QQuickWindow *>(output->window()) : nullptr;
    if (!window) {
        for (QWindow *w : app.topLevelWindows())
            if ((window = qobject_cast<QQuickWindow *>(w)))
                break;
    }
    if (!window || (profile.tablet && !output))
        return 1;
    apps.setCompositor(compositor);
    force.setCompositor(compositor);
    android.setCompositor(compositor);
    // wp_viewporter — окнам Android (docs/ANDROID.md): Waydroid кладёт слои приложения подповерхностями с обрезкой и
    // размером через него, без него — слои без обрезки, а фон окна не прозрачный (в QML этого типа нет)
    if (compositor)
        (new QWaylandViewporter(compositor))->initialize();
    // Касания — программам в самом начале кадра. Qt Quick отдаёт их окну программы в начале кадра оболочки, но
    // Wayland копит события до простоя потока интерфейса, и касание порой уходило одним пакетом с «рисуй дальше»
    // (frame callback, после отрисовки): программа начинала кадр, не прочтя касания, — кадр без сдвига, следом
    // двойной шаг (docs/SHELL.md, «Окна программ»). Синхронизация — поток отрисовки, поток интерфейса в это время
    // стоит: Wayland трогать можно (так же Qt зовёт здесь frameStarted).
    if (compositor)
        QObject::connect(window, &QQuickWindow::beforeSynchronizing, compositor, [compositor] {
            wl_display_flush_clients(compositor->display());
        }, Qt::DirectConnection);

    power.attach(window);
    // На планшете стол занимает панель; у телефона размер второго вывода задаёт зритель.
    SystemMonitor monitor;   // монитор системы — окно под индикатором подключения; живёт дольше сцены стола
    DesktopScreen desktop(&engine, app.applicationDirPath() + "/qml/desktop/Desktop.qml",
                          profile.tablet ? window->screen()->size() : QSize(1280, 720), profile.tablet ? density : 1.5,
                          profile.tablet ? window : nullptr, stats.sceneRotation());
    ctx->setContextProperty("naomiDesk", &desktop);   // масштаб стола — программам стола (Desktop.qml)
    monitor.setDesk(&desktop);
    ctx->setContextProperty("naomiMonitor", &monitor);
    // скриншоты стола (Shift+Alt+3 и 4, docs/DESKTOP.md, «Скриншоты»): миниатюра — image://deskshot/N
    auto *shot = new DeskShot(&desktop, &app);   // живёт дольше сцены стола (её корень удаляет ~DesktopScreen)
    engine.addImageProvider(QStringLiteral("deskshot"), shot->imageProvider());
    ctx->setContextProperty("naomiShot", shot);
    desktop.setPhoneWindow(window);
    new DeskProbe(&desktop, &web, &app);   // зонд стола для замеров по SSH (desk_probe.h, tools/deskbench)
    if (compositor)
        desktop.setCompositor(compositor, output);
    if (profile.tablet) {
        desktop.setNativeRefreshRate(window->screen()->refreshRate());
        QObject::connect(&stats, &DisplayStats::changed, &desktop, [&] { desktop.setNativeRefreshRate(stats.refreshHz()); });
        if (!desktop.showNative())
            return 1;
    }
    android.setDeskOutput(desktop.output());   // Android видит из выводов только стол (android_apps.h)
    android.setWinOutput(desktop.winOutput(), [&apps](qint64 pid) { return apps.deskApp(pid) == QLatin1String("win"); });
    if (!profile.tablet)
        streamService.attachDesktop(&desktop);
    streamService.attach(window, profile.tablet ? &desktop : nullptr, stats.sceneRotation());
    if (profile.tablet) {
        QObject::connect(&stats, &DisplayStats::sceneRotationChanged, &desktop, [&] {
            desktop.setSceneRotation(stats.sceneRotation());
            streamService.setNativeRotation(stats.sceneRotation());
        });
    }
    QObject::connect(&streamService, &StreamService::activity, &power, &Power::activity);
    // Стол без зрителя (docs/WEB.md, «Без зрителя»): зритель ушёл сам — ролики браузера на паузу, через 2 минуты
    // браузер заморожен; трансляцию выключили (плитка в шторке) — заморожен сразу: страницы, потом ядром — все
    // процессы программ стола (AppLauncher::freezeDesk, docs/DESKTOP.md, «Выключатель»)
    // Пока стол смотрят — программы стола на больших ядрах (AppLauncher::setDeskBoost, docs/WEB.md, «Большие ядра для
    // стола»): телефон тогда на зарядке у приставки или у Mac, а картинка большая
    // И весь телефон — «на полную»: пол частот ядер, оболочка в поднятой группе, видеочип 625 МГц, пороги корпуса
    // позже (naomid Perf, docs/DESKTOP.md, «Стол на полную»). Оболочка упала — naomid отпустит сам; naomid
    // перезапустился (у нового режима нет) — просим заново
    static bool full = false;
    auto holdFull = [] {
        QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.naomi.Phone"),
                                                        QStringLiteral("/org/naomi/Phone/Perf"),
                                                        QStringLiteral("org.naomi.Phone.Perf"),
                                                        QStringLiteral("HoldDesk"));
        m << full;
        QDBusConnection::systemBus().asyncCall(m);
    };
    QDBusServiceWatcher phoneBack(QStringLiteral("org.naomi.Phone"), QDBusConnection::systemBus(),
                                  QDBusServiceWatcher::WatchForRegistration);
    QObject::connect(&phoneBack, &QDBusServiceWatcher::serviceRegistered, &app, [holdFull] {
        if (full)
            holdFull();
    });
    auto deskState = [&] {
        web.setDesk(profile.tablet || streamService.enabled(), profile.tablet || streamService.connected());
        const bool watched = profile.pixel && streamService.connected() && streamService.desktop();
        apps.setDeskBoost(watched);
        if (watched != full) {
            full = watched;
            holdFull();
        }
    };
    QObject::connect(&streamService, &StreamService::changed, &web, deskState);
    QObject::connect(&web, &WebLink::freezeDesk, &apps, &AppLauncher::freezeDesk);
    QObject::connect(&web, &WebLink::freezeDesk, &android, &AndroidApps::freeze);   // и Android (docs/ANDROID.md)
    deskState();

    // Видеокодек у телефона один. Пока рядом могут понадобиться декодеры Android — у него есть окна и он не спит
    // (ролик в его приложении), — кодер трансляции оставляет им место (docs/STREAM.md, «Кодек на всех»). «Просмотру»
    // место не нужно: его декодер драйвер пускает и при полной скорости кодера (4K30 — проверено 20 ноября 2026)
    const auto decodersNear = [&streamService, &android] {
        streamService.setDecodersNear(android.state() == QLatin1String("on") && android.windows() && !android.asleep()
                                      && !android.frozen());
    };
    QObject::connect(&android, &AndroidApps::windowsChanged, &app, decodersNear);
    QObject::connect(&android, &AndroidApps::stateChanged, &app, decodersNear);
    QObject::connect(&android, &AndroidApps::asleepChanged, &app, decodersNear);
    QObject::connect(&android, &AndroidApps::frozenChanged, &app, decodersNear);
    if (trace)
        trace->attach(window);
    auto *unixSignals = new UnixSignals(&app);
    // SIGTERM: сначала программы (им для выхода нужен композитор), потом сама оболочка; Android гасит себя сам
    QObject::connect(unixSignals, &UnixSignals::terminate, &android, &AndroidApps::shutdown);
    QObject::connect(unixSignals, &UnixSignals::terminate, &apps, &AppLauncher::closeAll);
    QObject::connect(&apps, &AppLauncher::allClosed, &app, &QGuiApplication::quit);
    QObject::connect(unixSignals, &UnixSignals::screenshot, window, [window, &desktop] {
        takeScreenshot(window);
        saveImage(desktop.grab(), "/tmp/naomi-desk.png");   // и стол (tools/naomi shot desk)
    });
    // новая ширина кадра — тем же мягким выходом, с кодом «перезапусти» (служба naomi-shell:
    // RestartForceExitStatus=75)
    bool restart = false;
    QObject::connect(&stats, &DisplayStats::restartRequested, &apps, [&] {
        restart = true;
        apps.closeAll();
    });
    const int code = app.exec();
    return restart ? kRestartExitCode : code;
}
