// naomi-camera — нативная «Камера» PopkaPhone (Xiaomi Pad 6 / Halium 13).
// Поддерживает GUI-режим (Qt Quick поверх SHM Wayland + отдельный поток offscreen EGL/libcamera)
// и headless CLI-режимы (--snap, --torch) без Wayland (для Telegram-бота и скриптов).
#include "camera_engine.h"
#include "camera_viewfinder.h"
#include "unix_signals.h"

#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>

#include <cstdio>
#include <cstring>

namespace {

void printUsage()
{
    std::printf(
        "Usage: naomi-camera [options]\n"
        "  (no options)                               Launch interactive Qt Quick camera UI\n"
        "  --snap back|front --out <path>             Headless JPEG snapshot\n"
        "         [--zoom 0..99] [--flash off|auto|on] [--width 1920] [--height 1080]\n"
        "  --torch <on|off|seconds>                   Keep rear LED torch active until SIGTERM or timeout\n"
        "  --torch-seconds <seconds>                  Alias for --torch <seconds>\n"
        "  --help                                     Show this help\n");
}

int parseCliAndRunIfHeadless(int argc, char **argv, bool *handled)
{
    *handled = false;

    bool wantSnap = false;
    bool wantTorch = false;
    QString snapFacing = QStringLiteral("back");
    QString outPath;
    QString flash = QStringLiteral("off");
    int zoom = 0;
    int width = 1920;
    int height = 1080;
    QString torchArg = QStringLiteral("600");
    int torchSeconds = 600;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage();
            *handled = true;
            return 0;
        }
        if (std::strcmp(arg, "--snap") == 0) {
            wantSnap = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                snapFacing = QString::fromUtf8(argv[++i]);
        } else if (std::strcmp(arg, "--out") == 0 && i + 1 < argc) {
            outPath = QString::fromUtf8(argv[++i]);
        } else if (std::strcmp(arg, "--zoom") == 0 && i + 1 < argc) {
            zoom = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--flash") == 0 && i + 1 < argc) {
            flash = QString::fromUtf8(argv[++i]);
        } else if (std::strcmp(arg, "--width") == 0 && i + 1 < argc) {
            width = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--height") == 0 && i + 1 < argc) {
            height = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--torch") == 0) {
            wantTorch = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                torchArg = QString::fromUtf8(argv[++i]);
        } else if (std::strcmp(arg, "--torch-seconds") == 0 && i + 1 < argc) {
            wantTorch = true;
            torchArg = QString::fromUtf8(argv[++i]);
            torchSeconds = torchArg.toInt();
        }
    }

    if (wantSnap) {
        *handled = true;
        return CameraEngine::runHeadlessSnap(snapFacing, outPath, zoom, flash, width, height);
    }
    if (wantTorch) {
        *handled = true;
        return CameraEngine::runHeadlessTorch(torchArg, torchSeconds);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool handledHeadless = false;
    const int cliCode = parseCliAndRunIfHeadless(argc, argv, &handledHeadless);
    if (handledHeadless)
        return cliCode;

    // When running over SHM Wayland (QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=none), use the Qt Quick
    // software backend so the GUI never contends with CameraEngine's offscreen Android EGL context.
    if (qEnvironmentVariable("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION") == QLatin1String("none")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("naomi-camera"));
    app.setDesktopFileName(QStringLiteral("naomi-camera"));
    QGuiApplication::setFont(QFont(QStringLiteral("Google Sans")));

    qmlRegisterType<CameraViewfinder>("Naomi.Camera", 1, 0, "CameraViewfinder");

    CameraEngine cameraEngine;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("camera"), &cameraEngine);
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QGuiApplication::quit);

    engine.load(QUrl::fromLocalFile(app.applicationDirPath() + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *unixSignals = new UnixSignals(&app);
    QObject::connect(unixSignals, &UnixSignals::terminate, &app, [&app, &cameraEngine] {
        cameraEngine.stop();
        app.quit();
    });
    QObject::connect(unixSignals, &UnixSignals::pauseRequested, &cameraEngine, [&cameraEngine] {
        cameraEngine.setPaused(true);
    });
    QObject::connect(unixSignals, &UnixSignals::screenshot, &cameraEngine, [&cameraEngine] {
        cameraEngine.setPaused(false);
    });

    cameraEngine.start();
    const int rc = app.exec();
    cameraEngine.stop();
    return rc;
}
