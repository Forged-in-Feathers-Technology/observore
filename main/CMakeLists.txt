idf_component_register(
    SRCS
        "main.c"
        "observore_detect.c"
        "observore_track.c"
        "observore_mute.c"
        "observore_netcfg.c"
        "observore_auth.c"
        "observore_nvs.c"
        "observore_util.c"
        "observore_notify.c"
        "observore_notify_fmt.c"
        "observore_ble.c"
        "observore_wifi.c"
        "observore_web.c"
        "observore_led.c"
        "observore_improv.c"
        "observore_clock.c"
        "observore_history.c"
        "observore_wps.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        espressif__mdns
        esp-tls
        esp_http_client
        esp_wifi
        app_update
        nvs_flash
)

# The console is embedded gzipped and served with Content-Encoding: gzip.
# 25 KB of HTML becomes 8 KB of flash, and the page arrives faster on a device
# whose radio is shared with the thing it is trying to report.
set(CONSOLE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html")
set(CONSOLE_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${CONSOLE_GZ}"
    COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py"
            "${CONSOLE_SRC}" "${CONSOLE_GZ}"
    DEPENDS "${CONSOLE_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py"
    COMMENT "Compressing the console page"
    VERBATIM)
add_custom_target(observore_console_gz DEPENDS "${CONSOLE_GZ}")
add_dependencies(${COMPONENT_LIB} observore_console_gz)
target_add_binary_data(${COMPONENT_LIB} "${CONSOLE_GZ}" BINARY)
