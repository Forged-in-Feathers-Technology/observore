/* Argus -- a passive counter-surveillance detector for the XIAO ESP32S3.
 *
 * It listens, and it never transmits while listening: BLE scanning is passive
 * (no SCAN_REQ leaves the radio) and Wi-Fi sniffing is receive-only.  The one
 * time it transmits is when you ask it to, by holding the button to raise the
 * console SoftAP and read what it has seen.
 */

#include <inttypes.h>

#include "argus_ble.h"
#include "argus_led.h"
#include "argus_mute.h"
#include "argus_track.h"
#include "argus_web.h"
#include "argus_wifi.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "argus";

#define BUTTON_GPIO        ((gpio_num_t)CONFIG_ARGUS_BUTTON_GPIO)
#define BUTTON_HOLD_MS     1500
#define BUTTON_POLL_MS     50
#define HEARTBEAT_US       (30 * 1000000LL)

static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* BOOT is active low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    uint32_t held_ms = 0;
    bool acted = false;

    for (;;) {
        bool down = gpio_get_level(BUTTON_GPIO) == 0;

        if (!down) {
            held_ms = 0;
            acted = false;
        } else {
            held_ms += BUTTON_POLL_MS;
            /* Act on the hold, once, without waiting for release -- you should
             * be able to feel the mode change while still pressing. */
            if (held_ms >= BUTTON_HOLD_MS && !acted) {
                acted = true;
                bool to_console = argus_wifi_mode() != ARGUS_MODE_CONSOLE;
                if (to_console) {
                    argus_wifi_set_mode(ARGUS_MODE_CONSOLE);
                    argus_web_start();
                    argus_led_set_console(true);
                    ESP_LOGI(TAG, "console up: join \"%s\" (password \"%s\"), "
                                  "then open http://192.168.4.1/",
                             argus_wifi_ap_ssid(), argus_wifi_ap_password());
                } else {
                    argus_web_stop();
                    argus_wifi_set_mode(ARGUS_MODE_PATROL);
                    argus_led_set_console(false);
                    ESP_LOGI(TAG, "back on patrol");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Argus starting");

    argus_track_init();
    argus_led_init();

    /* NVS is brought up by argus_wifi_init(); the mute store reads from it. */
    ESP_ERROR_CHECK(argus_wifi_init());
    argus_mute_init();
    ESP_LOGI(TAG, "%zu mute rules loaded", argus_mute_count());
    ESP_ERROR_CHECK(argus_ble_start());

    xTaskCreate(button_task, "argus_btn", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "patrolling. hold the button %d ms for the console.",
             BUTTON_HOLD_MS);

    argus_level_t last_level = ARGUS_LEVEL_CLEAR;
    static argus_event_t found[16];
    int64_t last_heartbeat_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        argus_track_tick(now);

        /* Print each detection once, when it is first identified.  Repeat
         * sightings are counted but not reprinted, or a single beacon would
         * bury everything else in the log. */
        size_t n = argus_track_drain_new(found, sizeof(found) / sizeof(found[0]));
        for (size_t i = 0; i < n; i++) {
            const argus_event_t *e = &found[i];
            ESP_LOGW(TAG,
                     "%-16s %02X:%02X:%02X:%02X:%02X:%02X %4d dBm  via %-10s "
                     "%-13s  %s%s%s",
                     argus_class_name(e->cls),
                     e->mac[0], e->mac[1], e->mac[2],
                     e->mac[3], e->mac[4], e->mac[5],
                     e->rssi, argus_source_name(e->src),
                     argus_evidence_name(e->evidence), e->label,
                     e->detail[0] ? " / " : "", e->detail);
        }

        argus_status_t st;
        argus_track_status(&st, now);
        argus_led_set_level(st.level);

        if (st.level != last_level) {
            ESP_LOGW(TAG, "%s -> %s (score %u, %u devices)",
                     argus_level_name(last_level), argus_level_name(st.level),
                     st.score, st.device_count);
            last_level = st.level;
        }

        /* Heartbeat.  Without it, "nothing is out there" and "the radio is
         * not running" produce identical output: silence. */
        if (now - last_heartbeat_us >= HEARTBEAT_US) {
            last_heartbeat_us = now;
            ESP_LOGI(TAG, "%s | score %u | %u devices | %" PRIu32 " sightings | "
                          "%" PRIu32 "/%" PRIu32 " frames sniffed",
                     argus_level_name(st.level), st.score, st.device_count,
                     st.total_sightings, argus_wifi_sniffed_frames(),
                     argus_wifi_sniffer_calls());
        }

        if (argus_wifi_mode() == ARGUS_MODE_PATROL) {
            /* Blocks for the scan plus the sniff sweep.  BLE keeps running
             * underneath on the NimBLE host task throughout. */
            argus_wifi_patrol_cycle();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
