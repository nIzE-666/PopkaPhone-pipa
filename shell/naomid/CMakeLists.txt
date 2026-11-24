# naomid — служба железа НаомиФона (docs/NAOMID.md): системная служба на шине D-Bus, без интерфейса.
# Ставит её tools/naomi install: копия в /usr/local/lib/naomi (служба root не запускается из домашней папки).
add_executable(naomid main.cpp
    bus_object.cpp bus_object.h
    battery.cpp battery.h
    charge.cpp charge.h
    wifi.cpp wifi.h
    cell.cpp cell.h
    audio.cpp audio.h
    bluetooth.cpp bluetooth.h
    sensors.cpp sensors.h
    net.cpp net.h
    cable.cpp cable.h
    gpu.cpp gpu.h
    perf.cpp perf.h
    system.cpp system.h
    android.cpp android.h
    ${CMAKE_SOURCE_DIR}/app/dbus_plain.cpp ${CMAKE_SOURCE_DIR}/app/dbus_plain.h
    ${CMAKE_SOURCE_DIR}/app/unix_signals.cpp ${CMAKE_SOURCE_DIR}/app/unix_signals.h)
target_include_directories(naomid PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(naomid PRIVATE naomi-device Qt6::DBus)
