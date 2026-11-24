# naomi-camera — нативная «Камера» PopkaPhone (libhybris libcamera.so.1 + offscreen EGL + Qt Quick SHM)
pkg_check_modules(HYBRIS_CAMERA QUIET libcamera)

add_executable(naomi-camera
    main.cpp
    hybris_camera.h
    camera_engine.cpp
    camera_engine.h
    camera_viewfinder.cpp
    camera_viewfinder.h
    ${CMAKE_SOURCE_DIR}/app/unix_signals.cpp
    ${CMAKE_SOURCE_DIR}/app/unix_signals.h
)

target_include_directories(naomi-camera PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HYBRIS_CAMERA_INCLUDE_DIRS}
)

target_link_libraries(naomi-camera PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    PkgConfig::GLES
    ${CMAKE_DL_LIBS}
)

add_custom_target(naomi-camera-qml ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:naomi-camera>/qml
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml
            $<TARGET_FILE_DIR:naomi-camera>/qml
    DEPENDS naomi-camera
)
