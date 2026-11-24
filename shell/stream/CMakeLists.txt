# naomi-stream — трансляция экрана (docs/STREAM.md): буферы Android (gralloc), упаковка кадра в NV12 на видеочипе,
# аппаратный видеокодер Venus (V4L2). naomi-venc-bench — замер кодера тем же путём.
add_library(naomi-stream STATIC
    gralloc_buffer.cpp gralloc_buffer.h
    nv12_pack.cpp nv12_pack.h
    venus_encoder.cpp venus_encoder.h)
target_include_directories(naomi-stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(naomi-stream PUBLIC PkgConfig::GLES PRIVATE naomi-device)

add_executable(naomi-venc-bench venc_bench.cpp)
target_link_libraries(naomi-venc-bench PRIVATE naomi-stream)

# naomi-stream-service — служба трансляции в оболочке: захват кадра, поток кодера и связи, ввод зрителя и клавиатуры
# самого телефона, звук стола (naomiStream)
add_library(naomi-stream-service STATIC
    stream_service.cpp stream_service.h
    stream_input.cpp stream_input.h
    desk_test_input.cpp desk_test_input.h
    hard_keyboard.cpp hard_keyboard.h key_layout.h
    media_keys.cpp media_keys.h
    audio_capture.cpp audio_capture.h
    sound_history.h
    stream_protocol.h)
# закрытый заголовок клавиатуры Qt Wayland — модификаторы окну программы при ⌘-щелчке (stream_input.cpp);
# звук — libpulse системы по имени файла (заголовков нет, audio_capture.h)
target_link_libraries(naomi-stream-service PUBLIC naomi-stream Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::DBus
                      Qt6::WaylandCompositor Qt6::WaylandCompositorPrivate -l:libpulse.so.0)
