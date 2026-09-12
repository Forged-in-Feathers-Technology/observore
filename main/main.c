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
#include "argus_netcfg.h"
#include "argus_notify.h"
#include "argus_track.h"
#include "argus_web.h"
#include "argus_wifi.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "argus";

#define BUTTON_GPIO        ((gpio_num_t)CONFIG_ARGUS_BUTTON_GPIO)
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

static void enter_mode(argus_mode_t next)
{
    if (next == ARGUS_MODE_PATROL) {
        argus_web_stop();
        argus_wifi_set_mode(ARGUS_MODE_PATROL);
        argus_led_set_console(false);
        ESP_LOGI(TAG, "back on patrol");
        return;
    }

    argus_wifi_set_mode(next);
    /* set_mode falls back to patrol when the uplink cannot be joined, so ask
     * where we actually ended up rather than assuming. */
    if (argus_wifi_mode() == ARGUS_MODE_PATROL) {
        argus_led_set_console(false);
        /* Schedule the next attempt here.  Leaving it unset meant the main
         * loop saw a due time of zero and retried immediately, stalling the
         * boot for a second fifteen-second timeout back to back. */
        s_next_uplink_try_us = esp_timer_get_time() +
                               (int64_t)CONFIG_ARGUS_UPLINK_RETRY_S * 1000000;
        ESP_LOGW(TAG, "could not join the network -- patrolling, retrying in %ds",
                 CONFIG_ARGUS_UPLINK_RETRY_S);
        return;
    }

    argus_web_start();
    argus_led_set_console(true);
    if (argus_wifi_mode() == ARGUS_MODE_UPLINK) {
        ESP_LOGI(TAG, "console at http://%s/ on your network",
                 argus_wifi_uplink_ip());
    } else {
        ESP_LOGI(TAG, "console up: join \"%s\" (password \"%s\"), "
                      "then open http://192.168.4.1/",
                 argus_wifi_ap_ssid(), argus_wifi_ap_password());
    }
}

/* Short hold: alternate the two working modes.  With no network configured
 * there is only one sensible destination, the console, since that is where a
 * network gets configured. */
static void toggle_mode(void)
{
    if (!argus_netcfg_is_set()) {
        enter_mode(argus_wifi_mode() == ARGUS_MODE_CONSOLE ? ARGUS_MODE_PATROL
                                                           : ARGUS_MODE_CONSOLE);
        return;
    }
    if (argus_wifi_mode() == ARGUS_MODE_UPLINK) {
        s_patrol_by_choice = true;
        enter_mode(ARGUS_MODE_PATROL);
    } else {
        s_patrol_by_choice = false;
        s_next_uplink_try_us = 0;
        enter_mode(ARGUS_MODE_UPLINK);
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
                enter_mode(ARGUS_MODE_CONSOLE);
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
    argus_netcfg_init();
    argus_notify_init();
    ESP_LOGI(TAG, "%zu mute rules loaded", argus_mute_count());
    ESP_ERROR_CHECK(argus_ble_start());

    xTaskCreate(button_task, "argus_btn", 3072, NULL, 3, NULL);

#if CONFIG_ARGUS_WIFI_AUTOJOIN
    if (argus_netcfg_is_set()) {
        char ssid[ARGUS_SSID_LEN];
        argus_netcfg_ssid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "joining \"%s\"", ssid);
        enter_mode(ARGUS_MODE_UPLINK);
    }
#endif

    ESP_LOGI(TAG, "hold %d ms to swap patrol/uplink, %d ms for the console.",
             BUTTON_HOLD_MS, BUTTON_CONSOLE_MS);

    argus_level_t last_level = ARGUS_LEVEL_CLEAR;
    static argus_event_t found[16];
    int64_t last_heartbeat_us = 0;
    int64_t uplink_lost_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        argus_track_tick(now);

        /* Print each detection once, when it is first identified.  Repeat
         * sightings are counted but not reprinted, or a single beacon would
         * bury everything else in the log. */
        size_t n = argus_track_drain_new(found, sizeof(found) / sizeof(found[0]));
        for (size_t i = 0; i < n; i++) {
            const argus_event_t *e = &found[i];
            argus_notify_event(e);
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
            argus_notify_level(last_level, st.level, st.score);
            last_level = st.level;
        }

        /* Queued notices go out here, so a detection made while patrolling is
         * delivered the next time the uplink is up rather than lost. */
        argus_notify_pump();

        /* Uplink is the resting state once a network is configured.  Come back
         * to it by itself after a drop or a failed join, unless patrol was
         * chosen deliberately. */
        if (argus_netcfg_is_set() && !s_patrol_by_choice) {
            if (argus_wifi_mode() == ARGUS_MODE_UPLINK &&
                !argus_wifi_uplink_connected()) {
                if (uplink_lost_us == 0) {
                    uplink_lost_us = now;
                } else if (now - uplink_lost_us >
                           (int64_t)CONFIG_ARGUS_UPLINK_GRACE_S * 1000000) {
                    /* No network and no sniffer is the worst of both. */
                    ESP_LOGW(TAG, "uplink down for %ds -- patrolling",
                             CONFIG_ARGUS_UPLINK_GRACE_S);
                    uplink_lost_us = 0;
                    s_next_uplink_try_us =
                        now + (int64_t)CONFIG_ARGUS_UPLINK_RETRY_S * 1000000;
                    enter_mode(ARGUS_MODE_PATROL);
                }
            } else if (argus_wifi_mode() == ARGUS_MODE_UPLINK) {
                uplink_lost_us = 0;
            } else if (argus_wifi_mode() == ARGUS_MODE_PATROL &&
                       now >= s_next_uplink_try_us) {
                ESP_LOGI(TAG, "retrying the uplink");
                s_next_uplink_try_us =
                    now + (int64_t)CONFIG_ARGUS_UPLINK_RETRY_S * 1000000;
                enter_mode(ARGUS_MODE_UPLINK);
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
                     argus_level_name(st.level), st.score, st.device_count,
                     st.total_sightings, argus_wifi_sniffed_frames(),
                     argus_wifi_sniffer_calls(), argus_notify_pending(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
