/* Observore -- a passive counter-surveillance detector for the XIAO ESP32S3.
 *
 * It listens, and it never transmits while listening: BLE scanning is passive
 * (no SCAN_REQ leaves the radio) and Wi-Fi sniffing is receive-only.  The one
 * time it transmits is when you ask it to, by holding the button to raise the
 * console SoftAP and read what it has seen.
 */

#include <inttypes.h>

#include "observore_ble.h"
#include "observore_led.h"
#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_nvs.h"
#include "observore_notify.h"
#include "observore_track.h"
#include "observore_web.h"
#include "observore_wifi.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "observore";

#define BUTTON_GPIO        ((gpio_num_t)CONFIG_OBSERVORE_BUTTON_GPIO)
/* A short hold alternates the two modes you actually live in.  A long hold
 * raises the SoftAP, which is only needed for first-time setup or when you are
 * away from the configured network -- so it should not be in the way the rest
 * of the time. */
#define BUTTON_HOLD_MS     1500
#define BUTTON_CONSOLE_MS  4000
#define BUTTON_POLL_MS     50
#define HEARTBEAT_US       (15 * 1000000LL)

/* Set when patrol was chosen deliberately.  An explicit choice is not
 * second-guessed by the automatic return to uplink. */
static bool s_patrol_by_choice;
static int64_t s_next_uplink_try_us;

static void enter_mode(observore_mode_t next)
{
    if (next == OBSERVORE_MODE_PATROL) {
        observore_web_stop();
        observore_wifi_set_mode(OBSERVORE_MODE_PATROL);
        observore_led_set_console(false);
        ESP_LOGI(TAG, "back on patrol");
        return;
    }

    observore_wifi_set_mode(next);
    /* set_mode falls back to patrol when the uplink cannot be joined, so ask
     * where we actually ended up rather than assuming. */
    if (observore_wifi_mode() == OBSERVORE_MODE_PATROL) {
        observore_led_set_console(false);
        /* Schedule the next attempt here.  Leaving it unset meant the main
         * loop saw a due time of zero and retried immediately, stalling the
         * boot for a second fifteen-second timeout back to back. */
        s_next_uplink_try_us = esp_timer_get_time() +
                               (int64_t)CONFIG_OBSERVORE_UPLINK_RETRY_S * 1000000;
        ESP_LOGW(TAG, "could not join the network -- patrolling, retrying in %ds",
                 CONFIG_OBSERVORE_UPLINK_RETRY_S);
        return;
    }

    observore_web_start();
    observore_led_set_console(true);
    if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
        ESP_LOGI(TAG, "console at http://%s/ on your network",
                 observore_wifi_uplink_ip());
    } else {
        ESP_LOGI(TAG, "console up: join \"%s\" (password \"%s\"), "
                      "then open http://192.168.4.1/",
                 observore_wifi_ap_ssid(), observore_wifi_ap_password());
    }
}

/* Short hold: alternate the two working modes.  With no network configured
 * there is only one sensible destination, the console, since that is where a
 * network gets configured. */
static void toggle_mode(void)
{
    if (!observore_netcfg_is_set()) {
        enter_mode(observore_wifi_mode() == OBSERVORE_MODE_CONSOLE ? OBSERVORE_MODE_PATROL
                                                           : OBSERVORE_MODE_CONSOLE);
        return;
    }
    if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
        s_patrol_by_choice = true;
        enter_mode(OBSERVORE_MODE_PATROL);
    } else {
        s_patrol_by_choice = false;
        s_next_uplink_try_us = 0;
        enter_mode(OBSERVORE_MODE_UPLINK);
    }
}

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
            /* The short gesture fires on release, because a hold has to be
             * allowed to continue into the long gesture before it can be
             * judged short. */
            if (!acted && held_ms >= BUTTON_HOLD_MS) {
                ESP_LOGI(TAG, "button held %" PRIu32 " ms -- switching mode",
                         held_ms);
                toggle_mode();
            } else if (!acted && held_ms > 150) {
                /* Say what was actually seen.  A press a shade too short is
                 * otherwise indistinguishable from a button that is not wired
                 * up, and the only recourse is to guess and try again. */
                ESP_LOGW(TAG, "button held %" PRIu32 " ms -- %d ms needed",
                         held_ms, BUTTON_HOLD_MS);
            }
            held_ms = 0;
            acted = false;
        } else {
            held_ms += BUTTON_POLL_MS;
            /* The long gesture fires while still held, so you can feel that it
             * took without having to guess how long to keep holding. */
            if (held_ms >= BUTTON_CONSOLE_MS && !acted) {
                acted = true;
                ESP_LOGI(TAG, "button held %" PRIu32 " ms -- console", held_ms);
                enter_mode(OBSERVORE_MODE_CONSOLE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Observore starting");

    observore_track_init();
    observore_led_init();

    /* NVS is brought up by observore_wifi_init(); the mute store reads from it. */
    ESP_ERROR_CHECK(observore_wifi_init());
    /* Before anything reads its settings: the project was called Argus once,
     * and a device flashed with an older build keeps everything under that
     * name. */
    observore_nvs_migrate();
    observore_mute_init();
    observore_netcfg_init();
    observore_notify_init();
    ESP_LOGI(TAG, "%zu mute rules loaded", observore_mute_count());
    ESP_ERROR_CHECK(observore_ble_start());

    xTaskCreate(button_task, "observore_btn", 3072, NULL, 3, NULL);

#if CONFIG_OBSERVORE_WIFI_AUTOJOIN
    if (observore_netcfg_is_set()) {
        char ssid[OBSERVORE_SSID_LEN];
        observore_netcfg_ssid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "joining \"%s\"", ssid);
        enter_mode(OBSERVORE_MODE_UPLINK);
    }
#endif

    ESP_LOGI(TAG, "hold %d ms to swap patrol/uplink, %d ms for the console.",
             BUTTON_HOLD_MS, BUTTON_CONSOLE_MS);

    observore_level_t last_level = OBSERVORE_LEVEL_CLEAR;
    static observore_event_t found[16];
    int64_t last_heartbeat_us = 0;
    int64_t uplink_lost_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        observore_track_tick(now);

        /* Print each detection once, when it is first identified.  Repeat
         * sightings are counted but not reprinted, or a single beacon would
         * bury everything else in the log. */
        size_t n = observore_track_drain_new(found, sizeof(found) / sizeof(found[0]));
        for (size_t i = 0; i < n; i++) {
            const observore_event_t *e = &found[i];
            observore_notify_event(e);
            ESP_LOGW(TAG,
                     "%-16s %02X:%02X:%02X:%02X:%02X:%02X %4d dBm  via %-10s "
                     "%-13s  %s%s%s",
                     observore_class_name(e->cls),
                     e->mac[0], e->mac[1], e->mac[2],
                     e->mac[3], e->mac[4], e->mac[5],
                     e->rssi, observore_source_name(e->src),
                     observore_evidence_name(e->evidence), e->label,
                     e->detail[0] ? " / " : "", e->detail);
        }

        observore_status_t st;
        observore_track_status(&st, now);
        observore_led_set_level(st.level);

        if (st.level != last_level) {
            ESP_LOGW(TAG, "%s -> %s (score %u, %u devices)",
                     observore_level_name(last_level), observore_level_name(st.level),
                     st.score, st.device_count);
            observore_notify_level(last_level, st.level, st.score);
            last_level = st.level;
        }

        /* Queued notices go out here, so a detection made while patrolling is
         * delivered the next time the uplink is up rather than lost. */
        observore_notify_pump();

        /* Uplink is the resting state once a network is configured.  Come back
         * to it by itself after a drop or a failed join, unless patrol was
         * chosen deliberately. */
        if (observore_netcfg_is_set() && !s_patrol_by_choice) {
            if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK &&
                !observore_wifi_uplink_connected()) {
                if (uplink_lost_us == 0) {
                    uplink_lost_us = now;
                } else if (now - uplink_lost_us >
                           (int64_t)CONFIG_OBSERVORE_UPLINK_GRACE_S * 1000000) {
                    /* No network and no sniffer is the worst of both. */
                    ESP_LOGW(TAG, "uplink down for %ds -- patrolling",
                             CONFIG_OBSERVORE_UPLINK_GRACE_S);
                    uplink_lost_us = 0;
                    s_next_uplink_try_us =
                        now + (int64_t)CONFIG_OBSERVORE_UPLINK_RETRY_S * 1000000;
                    enter_mode(OBSERVORE_MODE_PATROL);
                }
            } else if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
                uplink_lost_us = 0;
            } else if (observore_wifi_mode() == OBSERVORE_MODE_PATROL &&
                       now >= s_next_uplink_try_us) {
                ESP_LOGI(TAG, "retrying the uplink");
                s_next_uplink_try_us =
                    now + (int64_t)CONFIG_OBSERVORE_UPLINK_RETRY_S * 1000000;
                enter_mode(OBSERVORE_MODE_UPLINK);
            }
        }

        /* Heartbeat.  Without it, "nothing is out there" and "the radio is
         * not running" produce identical output: silence. */
        if (now - last_heartbeat_us >= HEARTBEAT_US) {
            last_heartbeat_us = now;
            ESP_LOGI(TAG, "%s | score %u | %u devices | %" PRIu32 " sightings | "
                          "%" PRIu32 "/%" PRIu32 " frames | %zu queued | "
                          "heap %u free, "
                          "%u min, %u largest",
                     observore_level_name(st.level), st.score, st.device_count,
                     st.total_sightings, observore_wifi_sniffed_frames(),
                     observore_wifi_sniffer_calls(), observore_notify_pending(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }

        if (observore_wifi_mode() == OBSERVORE_MODE_PATROL) {
            /* Blocks for the scan plus the sniff sweep.  BLE keeps running
             * underneath on the NimBLE host task throughout. */
            observore_wifi_patrol_cycle();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
