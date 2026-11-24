# naomi-shell — оболочка НаомиФона. QML лежит рядом с программой (qml/), правится без сборки C++.
add_executable(naomi-shell main.cpp
    display_stats.cpp display_stats.h scene_rotation.h
    power.cpp power.h
    haptics.cpp haptics.h
    project_file.cpp project_file.h
    system_stats.cpp system_stats.h
    system_tools.cpp system_tools.h
    phone_client.cpp phone_client.h
    dbus_plain.cpp dbus_plain.h
    dev_trace.cpp dev_trace.h
    dev_screenshot.cpp dev_screenshot.h
    unix_signals.cpp unix_signals.h
    code_pin.cpp code_pin.h
    system_monitor.cpp system_monitor.h
    force_quit.cpp force_quit.h
    app_launcher.cpp app_launcher.h
    android_apps.cpp android_apps.h
    game_library.cpp game_library.h
    pad_keys.cpp pad_keys.h
    android_scroll.cpp android_scroll.h
    android_sound.cpp android_sound.h
    android_move.cpp android_move.h
    android_surface.cpp android_surface.h
    outline_cut.cpp outline_cut.h
    web_surface.cpp web_surface.h
    desktop_screen.cpp desktop_screen.h
    desk_shot.cpp desk_shot.h
    desk_probe.cpp desk_probe.h
    sound_equalizer.cpp sound_equalizer.h sound_spectrum.h
    web_link.cpp web_link.h
    term_screen.cpp term_screen.h
    term_link.cpp term_link.h
    term_view.cpp term_view.h
    shell_compositor.cpp shell_compositor.h)
target_include_directories(naomi-shell PRIVATE ${CMAKE_SOURCE_DIR}/display)
# ввод текста (модуль «naomi», исправление опечаток) — общая библиотека shell/input, как у программ;
# трансляция экрана — shell/stream, «Файлы» рабочего стола — shell/files; связь с браузером — сокет (Network)
target_link_libraries(naomi-shell PRIVATE naomi-device naomi-input naomi-stream-service naomi-files naomi-preview Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::QuickPrivate Qt6::DBus
                      Qt6::Network Qt6::WaylandCompositor Qt6::WaylandCompositorPrivate)

# QML копируется при каждой сборке (не только когда пересобирается C++); удалённые файлы тоже уходят. Шейдер видео
# «Просмотра» (GLSL 440) qsb переводит рядом, в nv12.frag.qsb — как morph.frag у Naomi.Ui (shell/CMakeLists.txt).
# Шейдеры сглаженного края поверхности по контуру окна (outline_cut.cpp) — рядом с программой, в shaders/
add_custom_target(naomi-shell-qml ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:naomi-shell>/qml
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml
            $<TARGET_FILE_DIR:naomi-shell>/qml
    COMMAND $<TARGET_FILE:Qt6::qsb> --glsl "100 es,120,150"
            -o $<TARGET_FILE_DIR:naomi-shell>/qml/desktop/preview/shaders/nv12.frag.qsb
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/desktop/preview/shaders/nv12.frag
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:naomi-shell>/shaders
    COMMAND $<TARGET_FILE:Qt6::qsb> --batchable --glsl "100 es,120,150"   # вершинный — с вариантом для пакетов сцены Qt Quick
            -o $<TARGET_FILE_DIR:naomi-shell>/shaders/outline.vert.qsb
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/outline.vert
    COMMAND $<TARGET_FILE:Qt6::qsb> --glsl "100 es,120,150" -o $<TARGET_FILE_DIR:naomi-shell>/shaders/outline.frag.qsb
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/outline.frag
    COMMAND $<TARGET_FILE:Qt6::qsb> --glsl "100 es,120,150" -o $<TARGET_FILE_DIR:naomi-shell>/shaders/solid.frag.qsb
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.frag
    DEPENDS naomi-shell)
