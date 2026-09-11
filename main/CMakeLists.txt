idf_component_register(
    SRCS
        "main.c"
        "argus_detect.c"
        "argus_track.c"
        "argus_mute.c"
        "argus_ble.c"
        "argus_wifi.c"
        "argus_web.c"
        "argus_led.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
    EMBED_FILES "www/index.html"
)
