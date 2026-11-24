# eglfs_naomi — драйвер экрана Qt: HWC2 через libhybris, режимы панели, учёт кадров.
# Подключается так: QT_QPA_PLATFORM=eglfs QT_QPA_EGLFS_INTEGRATION=eglfs_naomi,
# QT_PLUGIN_PATH=<сборка>/plugins (tools/naomi run делает это сам).
pkg_check_modules(HYBRIS_EGL REQUIRED IMPORTED_TARGET hybris-egl-platform)
add_library(eglfs_naomi MODULE
    naomi_hwc.cpp naomi_hwc.h
    present_timer.cpp present_timer.h
    naomi_display.h
    eglfs_naomi.json)
target_link_libraries(eglfs_naomi PRIVATE
    naomi-device Qt6::EglFSDeviceIntegrationPrivate Qt6::GuiPrivate PkgConfig::HYBRIS PkgConfig::HYBRIS_EGL
    gralloc)   # hybris_gralloc_lock — залить чёрный кадр (naomi_hwc.cpp, blankFrame)
# eglfs в Qt собран с QT_EGL_NO_X11: без него qt_egl_p.h включает USE_X11, EGLNativeDisplayType
# становится _XDisplay*, и подписи виртуальных функций QEglFSDeviceIntegration не совпадут с
# библиотекой (плагин не загрузится: undefined symbol …createDisplayEP9_XDisplay).
target_compile_definitions(eglfs_naomi PRIVATE QT_EGL_NO_X11)
set_target_properties(eglfs_naomi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/egldeviceintegrations)
