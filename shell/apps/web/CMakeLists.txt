# naomi-web — «Браузер»: Naomi Web с НаомиОС (docs/WEB.md), отдельный процесс, клиент Wayland оболочки
# (запускает её оболочка: app_launcher). Движок — Qt WebEngine системы (Chromium 134); полосу рисует стол.
add_executable(naomi-web main.cpp web_bridge.cpp web_bridge.h web_store.cpp web_store.h web_filter.cpp web_filter.h
    google_auth.h ${CMAKE_SOURCE_DIR}/app/unix_signals.cpp ${CMAKE_SOURCE_DIR}/app/unix_signals.h)
# заголовков Qt WebEngine в среде сборки нет — связываем с библиотеками системы напрямую (main.cpp); три заголовка
# API перехватчика запросов — в third_party (README.md там)
target_include_directories(naomi-web PRIVATE ${CMAKE_SOURCE_DIR}/app ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
find_library(QT_WEBENGINE_QUICK NAMES libQt6WebEngineQuick.so.6 REQUIRED)
find_library(QT_WEBENGINE_CORE NAMES libQt6WebEngineCore.so.6 REQUIRED)
target_link_libraries(naomi-web PRIVATE naomi-input Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::Sql
    ${QT_WEBENGINE_QUICK} ${QT_WEBENGINE_CORE})

add_custom_target(naomi-web-qml ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:naomi-web>/qml
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml
            $<TARGET_FILE_DIR:naomi-web>/qml
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:naomi-web>/scripts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:naomi-web>/scripts
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:naomi-web>/newtab
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/newtab
            $<TARGET_FILE_DIR:naomi-web>/newtab
    DEPENDS naomi-web)
