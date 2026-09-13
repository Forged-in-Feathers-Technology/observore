/* Observore -- a passive counter-surveillance detector for the XIAO ESP32S3.
 *
 * It listens, and it never transmits while listening: BLE scanning is passive
 * (no SCAN_REQ leaves the radio) and Wi-Fi sniffing is receive-only.  The one
 * time it transmits is when you ask it to, by holding the button to raise the
 * console SoftAP and read what it has seen.
 */

#include <inttypes.h>

#include "observore_auth.h"
#include "observore_ble.h"
#include <limits.h>

#include "observore_clock.h"
#include "observore_history.h"
#include "observore_improv.h"
#include "observore_led.h"
#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_nvs.h"
#include "observore_util.h"
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

/* Observore alternates between patrolling and being on the network.  All
 * Wi-Fi detection -- the access-point scan and the promiscuous sniff both --
 * runs only while patrolling, so a device parked permanently on the uplink is
 * a BLE-only detector.  Alternating keeps the full sensor and still delivers
 * notifications and a reachable console. */
static void enter_mode(observore_mode_t next);

static int64_t s_mode_since_us;
static int64_t s_next_uplink_try_us;
static observore_level_t s_last_level = OBSERVORE_LEVEL_CLEAR;

static void enter_mode(observore_mode_t next)
{
    s_mode_since_us = esp_timer_get_time();

    if (next == OBSERVORE_MODE_PATROL) {
        observore_web_stop();
        /* Patrol is the fallback, so there is nowhere further to fall back to
         * and nothing to decide -- but it must not fail silently. The mode is
         * left unapplied, so the next cycle retries from a stopped driver;
         * saying so is what stops a device that has quietly gone deaf from
         * looking like a quiet neighbourhood. */
        esp_err_t err = observore_wifi_set_mode(OBSERVORE_MODE_PATROL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not enter patrol (%s) -- retrying next cycle",
                     esp_err_to_name(err));
        }
        observore_led_set_console(false);
        ESP_LOGI(TAG, "back on patrol");
        return;
    }

    /* The driver applies exactly what it is asked and reports failure, so the
     * decision about what to do instead is made here, next to the LED, the
     * console and the retry backoff that it affects. */
    esp_err_t err = observore_wifi_set_mode(next);
    if (err != ESP_OK) {
        /* Scheduling the next attempt matters: leaving it unset meant the main
         * loop saw a due time of zero and retried immediately, stalling boot
         * for a second fifteen-second timeout back to back. */
        s_next_uplink_try_us = esp_timer_get_time() +
                               (int64_t)CONFIG_OBSERVORE_UPLINK_RETRY_S * 1000000;
        ESP_LOGW(TAG, "could not join the network (%s) -- patrolling, "
                      "retrying in %ds",
                 esp_err_to_name(err), CONFIG_OBSERVORE_UPLINK_RETRY_S);
        enter_mode(OBSERVORE_MODE_PATROL);
        return;
    }

    /* Somebody is about to read this, so make sure what they see is also what
     * survives the next power cut. Forced rather than rate limited: opening
     * the console is a deliberate act, not a timer. */
    observore_history_flush(true);

    observore_web_start();
    observore_led_set_console(true);
    if (next == OBSERVORE_MODE_UPLINK) {
        ESP_LOGI(TAG, "console at http://%s/ or http://%s/ on your network",
                 observore_wifi_uplink_ip(), observore_wifi_hostname());
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
    /* The button means "switch now"; alternation carries on from there rather
     * than being disabled, so one press can never strand the device in a mode
     * it will not leave. */
    if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
        enter_mode(OBSERVORE_MODE_PATROL);
    } else {
        s_next_uplink_try_us = 0;   /* an explicit ask clears any backoff */
        enter_mode(OBSERVORE_MODE_UPLINK);
    }
}

/* Report whatever has been found: the serial log, the notifier queue and the
 * LED.  Called from the main loop and again between patrol channel dwells, so a
 * detection reaches the LED and the push queue within a few hundred
 * milliseconds instead of waiting out the eight-second sweep. */
static observore_status_t publish(void)
{
    static observore_event_t found[16];
    int64_t now = esp_timer_get_time();

    /* Each detection is reported once, when first identified.  Repeat
     * sightings are counted but not reprinted, or one beacon would bury
     * everything else. */
    size_t n = observore_track_drain_new(found, OBSERVORE_ARRLEN(found));
    for (size_t i = 0; i < n; i++) {
        const observore_event_t *e = &found[i];
        observore_history_note(e);
        observore_notify_event(e);
        char macbuf[OBSERVORE_MAC_STR_LEN];
        ESP_LOGW(TAG, "%-16s %s %4d dBm  via %-10s %-13s  %s%s%s",
                 observore_class_name(e->cls),
                 observore_mac_str(e->mac, macbuf),
                 e->rssi, observore_source_name(e->src),
                 observore_evidence_name(e->evidence), e->label,
                 e->detail[0] ? " / " : "", e->detail);
    }

    observore_status_t st;
    observore_track_status(&st, now);
    observore_led_set_level(st.level);

    if (st.level != s_last_level) {
        ESP_LOGW(TAG, "%s -> %s (score %u, %u devices)",
                 observore_level_name(s_last_level), observore_level_name(st.level),
                 st.score, st.device_count);
        observore_notify_level(s_last_level, st.level, st.score);
        s_last_level = st.level;
    }
    return st;
}

static void publish_void(void)
{
    (void)publish();
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
    /* Started here rather than on the first uplink: it renews its servers on
     * every new IP, so one init covers every window the device is associated
     * for, and the times it hands back are retroactive anyway. */
    observore_clock_init();
    observore_mute_init();
    observore_history_init();
    observore_netcfg_init();
    observore_auth_init();
    observore_notify_init();
    ESP_LOGI(TAG, "%zu mute rules loaded", observore_mute_count());
    /* Printed at boot, not only when the console comes up: you need it before
     * you can join, and the serial log is the one place it is safe to put it.
     * It is deliberately NOT exposed over the network -- serving the console's
     * own password from the console would turn "someone was on the LAN once"
     * into "someone can join the SoftAP in range, indefinitely". */
    ESP_LOGW(TAG, "console SoftAP: \"%s\"  password: %s",
             observore_wifi_ap_ssid(), observore_wifi_ap_password());
    ESP_ERROR_CHECK(observore_ble_start());

    xTaskCreate(button_task, "observore_btn", 3072, NULL, 3, NULL);

    /* Started even when a network is already configured: re-provisioning a
     * device that has moved house is the same problem as provisioning a new
     * one, and it only listens on a cable somebody has physically attached. */
    observore_improv_init();

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

    int64_t last_heartbeat_us = 0;
    int64_t uplink_lost_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        observore_track_tick(now);

        observore_status_t st = publish();

        /* Say when the internal-heap low-water mark moves, not just what it
         * ended up at.
         *
         * A device left running overnight came back reporting a minimum of 176
         * bytes free -- an order of magnitude below the 1.4 KB that once left
         * the SoftAP unable to answer an ARP request. The watermark alone says
         * it nearly died and nothing about when or during what, which makes it
         * a puzzle rather than a lead. Logging the moment it drops, with the
         * mode and the queue depth, turns the next soak into evidence. */
        {
            static unsigned s_reported_min = UINT_MAX;
            unsigned low = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            if (low + 2048 < s_reported_min) {
                s_reported_min = low;
                ESP_LOGW(TAG, "internal heap low-water fell to %u bytes "
                              "(%s, %zu queued, largest block %u)",
                         low, observore_mode_name(observore_wifi_mode()),
                         observore_notify_pending(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            }
        }

        /* Queued notices go out here, so a detection made while patrolling is
         * delivered the next time the uplink is up rather than lost. */
        observore_notify_pump();

        /* Rate limited inside, and a no-op unless something structural
         * changed -- a new classification, not another sighting of a device
         * already recorded. Called every pass so the limiter, rather than this
         * loop, decides when a write is due. */
        observore_history_flush(false);

        if (now - last_heartbeat_us >= HEARTBEAT_US) {
            last_heartbeat_us = now;
            ESP_LOGI(TAG, "%s | %s | score %u | %u devices | %" PRIu32
                          " sightings | %" PRIu32 "/%" PRIu32 " frames | "
                          "%zu queued | heap %u free, %u min, %u largest",
                     observore_mode_name(observore_wifi_mode()),
                     observore_level_name(st.level), st.score, st.device_count,
                     st.total_sightings, observore_wifi_sniffed_frames(),
                     observore_wifi_sniffer_calls(), observore_notify_pending(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }

        /* Alternate patrol and uplink.  Console mode is never alternated out
         * of: it is chosen deliberately, usually because the network is not
         * reachable and the SoftAP is the only way in. */
        if (observore_netcfg_is_set() &&
            observore_wifi_mode() != OBSERVORE_MODE_CONSOLE) {
            int64_t in_mode = now - s_mode_since_us;

            if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
                if (!observore_wifi_uplink_connected()) {
                    if (uplink_lost_us == 0) {
                        uplink_lost_us = now;
                    } else if (now - uplink_lost_us >
                               (int64_t)CONFIG_OBSERVORE_UPLINK_GRACE_S * 1000000) {
                        ESP_LOGW(TAG, "uplink down for %ds -- patrolling",
                                 CONFIG_OBSERVORE_UPLINK_GRACE_S);
                        uplink_lost_us = 0;
                        s_next_uplink_try_us =
                            now + (int64_t)CONFIG_OBSERVORE_UPLINK_RETRY_S * 1000000;
                        enter_mode(OBSERVORE_MODE_PATROL);
                    }
                } else {
                    uplink_lost_us = 0;
                    int64_t idle = now - observore_web_last_request_us();
                    bool console_in_use =
                        observore_web_last_request_us() != 0 &&
                        idle < (int64_t)CONFIG_OBSERVORE_CONSOLE_IDLE_S * 1000000;
                    /* Hold the window open while the console is being read,
                     * but only so far.  The page polls every two seconds, so
                     * a tab left open would otherwise keep the device on the
                     * uplink forever -- and on the uplink it does no Wi-Fi
                     * detection at all.  A user interface must not be able to
                     * blind the detector indefinitely, so the hold has a hard
                     * ceiling. */
                    bool overdue =
                        in_mode >= (int64_t)CONFIG_OBSERVORE_UPLINK_MAX_S * 1000000;
                    bool window_done =
                        in_mode >= (int64_t)CONFIG_OBSERVORE_UPLINK_WINDOW_S * 1000000;

                    if (overdue || (window_done && !console_in_use &&
                                    observore_notify_pending() == 0)) {
                        ESP_LOGI(TAG, "uplink window done (%llds%s) -- patrolling",
                                 (long long)(in_mode / 1000000),
                                 overdue ? ", capped" : "");
                        enter_mode(OBSERVORE_MODE_PATROL);
                    }
                }
            } else if (now >= s_next_uplink_try_us &&
                       in_mode >= (int64_t)CONFIG_OBSERVORE_PATROL_WINDOW_S * 1000000) {
                ESP_LOGI(TAG, "patrol window done -- visiting the uplink");
                enter_mode(OBSERVORE_MODE_UPLINK);
            }
        }

        if (observore_wifi_mode() == OBSERVORE_MODE_PATROL) {
            /* Blocks for the scan plus the sniff sweep.  BLE keeps running
             * underneath on the NimBLE host task throughout. */
            observore_wifi_patrol_cycle(publish_void);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
