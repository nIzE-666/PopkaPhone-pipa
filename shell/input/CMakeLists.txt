# naomi-input — ввод текста НаомиФона одной библиотекой для оболочки и программ (docs/KEYBOARD.md): модуль
# ввода Qt «naomi» (keyboard_context, статический плагин: подключается Q_IMPORT_PLUGIN) и исправление
# опечаток (spell). Клавиатура (QML) — в общем модуле Naomi.Ui: Keyboard и KeyboardPanel.
add_library(naomi-input STATIC
    keyboard_context.cpp keyboard_context.h keyboard_context.json keyboard_logic.h
    spell.cpp spell.h)
target_include_directories(naomi-input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(naomi-input PRIVATE QT_STATICPLUGIN)
target_link_libraries(naomi-input PUBLIC Qt6::Gui Qt6::GuiPrivate Qt6::DBus)
