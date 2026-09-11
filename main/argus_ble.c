#include <string.h>

#include "argus_ble.h"
#include "sdkconfig.h"
#include "argus_track.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "argus.ble";

/* BLE and Wi-Fi share one radio, and the coexistence arbiter divides it by
 * BLE's duty cycle.  The relationship is sharply non-linear -- measured on a
 * XIAO ESP32S3 over 40 s in a flat with 15 APs in range:
 *
 *   window/interval   duty     BLE sightings   Wi-Fi frames sniffed
 *   100/100           100%          1490                  2
 *    60/160          37.5%           925                199
 *    45/160          28%             747                223
 *    30/160         18.75%           608                278
 *
 * A continuously-open BLE receiver does not merely slow the sniffer down, it
 * starves it outright.  The default trades a third of BLE throughput for a
 * sniffer that works at all.  Raise the window if BLE is all you care about;
 * lower it if you are hunting Remote ID beacons. */
#define SCAN_ITVL_MS   CONFIG_ARGUS_BLE_SCAN_INTERVAL_MS
#define SCAN_WINDOW_MS CONFIG_ARGUS_BLE_SCAN_WINDOW_MS
#define MS_TO_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

static uint8_t s_own_addr_type;

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    const struct ble_gap_disc_desc *d = &event->disc;
    argus_observation_t obs = {
        .mac         = d->addr.val,
        .src         = ARGUS_SRC_BLE,
        .rssi        = (int8_t)d->rssi,
        .channel     = 0,
        .addr_random = (d->addr.type == BLE_ADDR_RANDOM ||
                        d->addr.type == BLE_ADDR_RANDOM_ID),
        .adv         = d->data,
        .adv_len     = d->length_data,
    };
    argus_track_observe(&obs, esp_timer_get_time());
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl          = MS_TO_UNITS(SCAN_ITVL_MS),
        .window        = MS_TO_UNITS(SCAN_WINDOW_MS),
        .filter_policy = 0,
        .limited       = 0,
        /* Passive: request no scan response.  This is what makes the device
         * undetectable to the equipment it is listening for. */
        .passive       = 1,
        /* Duplicate filtering would hide exactly the repeat sightings the
         * follower heuristic counts, so it stays off. */
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                          on_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "passive scan running (%d ms window / %d ms interval)",
                 SCAN_WINDOW_MS, SCAN_ITVL_MS);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t argus_ble_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
