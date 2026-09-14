#include <string.h>

#include "observore_ble.h"
#include "sdkconfig.h"
#include "observore_track.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "observore.ble";

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
#define SCAN_ITVL_MS   CONFIG_OBSERVORE_BLE_SCAN_INTERVAL_MS
#define SCAN_WINDOW_MS CONFIG_OBSERVORE_BLE_SCAN_WINDOW_MS
#define MS_TO_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

static uint8_t s_own_addr_type;

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    const struct ble_gap_disc_desc *d = &event->disc;

    /* NimBLE stores an address least significant byte first, which is the order
     * it travels in on air. Everything else here reads a MAC the way it is
     * written down, most significant byte first, so it has to be turned round
     * exactly once and this is the only place that sees the raw form.
     *
     * Getting this wrong is quiet rather than loud: the address still looks
     * like an address, still compares equal to itself, and still mutes
     * correctly. What it stops is the OUI lookup, because the vendor prefix
     * ends up in the last three bytes, so every BLE device reads as having an
     * unknown vendor rather than as being wrong. */
    uint8_t mac[OBSERVORE_MAC_LEN];
    for (size_t i = 0; i < OBSERVORE_MAC_LEN; i++) {
        mac[i] = d->addr.val[OBSERVORE_MAC_LEN - 1 - i];
    }

    observore_observation_t obs = {
        .mac         = mac,
        .src         = OBSERVORE_SRC_BLE,
        .rssi        = (int8_t)d->rssi,
        .channel     = 0,
        .addr_random = (d->addr.type == BLE_ADDR_RANDOM ||
                        d->addr.type == BLE_ADDR_RANDOM_ID),
        .adv         = d->data,
        .adv_len     = d->length_data,
    };
    observore_track_observe(&obs, esp_timer_get_time());
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

esp_err_t observore_ble_start(void)
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

esp_err_t observore_ble_stop(void)
{
    /* Idle the controller before taking it apart. EALREADY means there was no
     * scan running, which is the state wanted. */
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "could not stop the scan before shutdown: %d", rc);
    }

    rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
        return ESP_FAIL;
    }

    /* Release the controller directly rather than through nimble_port_deinit().
     *
     * That call reaches ble_hs_deinit(), which references ble_sm_deinit(), which
     * is not compiled in an observer-only build -- NIMBLE_BLE_SM is gated on a
     * connecting role and this device never connects to anything. The result is
     * a link error rather than a runtime one, so it cannot be worked around at
     * run time. Taking the controller down by hand frees the memory that
     * actually matters: the host's own structures are small beside the
     * controller's DMA-capable buffers.
     *
     * This is one way. Nothing restarts BLE afterwards -- a finished update
     * reboots, and a failed one reboots too, which is why there is no resume. */
    esp_err_t err = esp_bt_controller_disable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "controller disable: %s", esp_err_to_name(err));
    }
    err = esp_bt_controller_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "controller deinit: %s", esp_err_to_name(err));
    }
    esp_bt_mem_release(ESP_BT_MODE_BLE);

    ESP_LOGI(TAG, "BLE stopped for this boot");
    return ESP_OK;
}
