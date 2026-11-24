# НаомиФон — оболочка. Собирается на телефоне в среде сборки (docs/DEVELOPMENT.md):
#   tools/naomi build
cmake_minimum_required(VERSION 3.22)
project(naomiphone LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# закрытые заголовки Qt берём сознательно (драйвер экрана); Qt в среде и в системе — одна сборка 6.10.2
set(QT_NO_PRIVATE_MODULE_WARNING ON)
add_compile_options(-Wno-psabi)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui GuiPrivate Qml Quick QuickPrivate DBus Network Sql WaylandCompositor
                                          WaylandCompositorPrivate
                                          EglFSDeviceIntegrationPrivate ShaderTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HYBRIS REQUIRED IMPORTED_TARGET libhwc2 hwcomposer-egl libhardware)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET egl glesv2)

add_library(naomi-device STATIC device_profile.cpp device_profile.h)
set_target_properties(naomi-device PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(naomi-device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(naomi-device PUBLIC Qt6::Core)

add_subdirectory(display)
add_subdirectory(input)
add_subdirectory(stream)
add_subdirectory(files)
add_subdirectory(preview)
add_subdirectory(app)
add_subdirectory(apps/web)
add_subdirectory(apps/game)
add_subdirectory(apps/camera)
add_subdirectory(term)
add_subdirectory(naomid)

# общий QML-модуль Naomi.Ui (тема, переключатели) — для оболочки и программ: <сборка>/qml/Naomi/Ui.
# Копируется при каждой сборке, удалённые файлы тоже уходят. Шейдеры (shaders/*.frag, GLSL 440) qsb переводит
# для OpenGL ES видеочипа телефона — рядом, в *.frag.qsb (их и берёт ShaderEffect); версии — те же, что у
# встроенного вершинного шейдера ShaderEffect (100 es, 120, 150): иначе «shader version mismatch»
add_custom_target(naomi-ui-qml ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/qml
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui ${CMAKE_BINARY_DIR}/qml
    COMMAND $<TARGET_FILE:Qt6::qsb> --glsl "100 es,120,150" -o ${CMAKE_BINARY_DIR}/qml/Naomi/Ui/shaders/morph.frag.qsb
            ${CMAKE_SOURCE_DIR}/ui/Naomi/Ui/shaders/morph.frag)
