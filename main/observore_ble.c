#include <string.h>

#include "observore_ble.h"
#include "observore_meshkey.h"
#include "observore_peer.h"
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
    int64_t now = esp_timer_get_time();

    /* A warning from another node is kept as a warning as well as being
     * classified as the peer that sent it. The classifier stays pure and
     * says what the finding looks like; the standing record of who warned
     * about what, with replay and ageing, lives in observore_peer. */
    observore_peer_warning_t warn;
    if (observore_peer_from_advert(d->data, d->length_data, &warn)) {
        /* Whether this one came from a node that shares the household key.
         *
         * Checked over the bytes that arrived, not over a re-encoding of the
         * parsed fields: any difference between sender and receiver -- a
         * clamped age, a quantised coordinate -- would fail verification and
         * read as a forgery. observore_peer_tag_span() hands back exactly
         * what was signed.
         *
         * The payload is found again here rather than carried out of the
         * parser because the parser is pure and host-tested and has no
         * business knowing about keys. */
        size_t mfg_len = 0;
        const uint8_t *mfg = observore_adv_field(d->data, d->length_data,
                                                 0xFF, &mfg_len);
        if (mfg && mfg_len > 2) {
            const uint8_t *tag = NULL;
            size_t span = observore_peer_tag_span(mfg + 2, mfg_len - 2, &tag);
            if (span > 0 && observore_meshkey_verify(mfg + 2, span, tag)) {
                warn.trusted = true;
            }
        }
        observore_peer_note(&warn, now);
    }

    observore_track_observe(&obs, now);
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

static bool s_synced;

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
    s_synced = true;
    start_scan();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}


#if CONFIG_OBSERVORE_MESH_TX

/* Warning other nodes, which is the only thing this device ever transmits
 * while patrolling (#132).
 *
 * A burst rather than a standing advert. A node advertising continuously is
 * findable continuously, and the point is to pass on a finding, not to
 * announce a presence -- so it speaks when it has something to say and is
 * otherwise exactly as quiet as a build without this compiled in.
 *
 * The address is random and regenerated for every burst. The payload's node
 * id is sixteen bits and deliberately coarse; leaving the device's own public
 * BLE address on the air would undo that, because an address that does not
 * change says "this same box was also here yesterday" to anybody keeping a
 * list. A fresh non-resolvable address each time says only "an Observore is
 * near", which is the claim being made on purpose.
 *
 * Non-connectable and non-discoverable: there is nothing to connect to, and a
 * scan response would be a second emission answering a stranger's question.
 */
#define WARN_BURST_MS      180
#define WARN_ITVL_MIN_MS    40
#define WARN_ITVL_MAX_MS    60

static uint16_t s_warn_seq;

bool observore_ble_warn(const observore_peer_warning_t *w)
{
    if (!w || !s_synced) {
        return false;
    }

    uint8_t element[31];
    observore_peer_warning_t out = *w;
    out.seq = ++s_warn_seq;

    /* Signed where there is a household key, and two passes because the tag
     * covers the bytes before it -- the flag that says a tag is there
     * included. Lay the element out with a placeholder, then compute the tag
     * over the span that arrives and write it in. */
    static const uint8_t PLACEHOLDER[OBSERVORE_PEER_TAG_LEN] = {0};
    bool sign = observore_meshkey_present();
    size_t n = observore_peer_advert_tagged(&out, sign ? PLACEHOLDER : NULL,
                                            element, sizeof(element));
    if (n == 0) {
        return false;
    }
    if (sign) {
        const uint8_t *at = NULL;
        size_t span = observore_peer_tag_span(element + 4, n - 4, &at);
        uint8_t tag[OBSERVORE_PEER_TAG_LEN];
        if (span > 0 && observore_meshkey_tag(element + 4, span, tag)) {
            memcpy(element + 4 + span, tag, sizeof(tag));
        } else {
            /* Could not sign. Sent untagged rather than carrying a
             * placeholder: a tag of zeroes is still a tag to a receiver, and
             * one that fails to verify is indistinguishable from a forgery.
             * Better to be a stranger than to look like an attacker. */
            n = observore_peer_advert(&out, element, sizeof(element));
            if (n == 0) {
                return false;
            }
        }
    }

    /* A fresh random address per burst. ble_hs_id_gen_rnd(1, ...) asks for a
     * non-resolvable private address, which is the one kind that carries no
     * identity at all -- it cannot be resolved back to this device by anyone,
     * including a receiver we would have liked to be recognised by. That is
     * the right trade: the node id in the payload is what identifies us, at
     * sixteen bits, and it is ours to choose. */
    /* Stop scanning before touching the address.
     *
     * The controller refuses LE Set Random Address while a scan, an advert or
     * a connection attempt is running -- the Bluetooth spec says so, and it
     * arrives here as 524: 0x20C, the HCI error base plus 12, "command
     * disallowed". This device scans forever, so every attempt failed, and
     * the first version of the code ignored the return value and asked to
     * advertise from an address that had never been set.
     *
     * So the radio goes deaf for the few milliseconds it takes to issue three
     * HCI commands. That is the honest shape of a single radio: it cannot
     * listen while it arranges to speak. The alternative was setting one
     * address at startup and keeping it for the life of the boot, which costs
     * the rotation the burst was designed around. */
    ble_gap_adv_stop();
    ble_gap_disc_cancel();

    ble_addr_t addr;
    int arc = ble_hs_id_gen_rnd(1, &addr);
    if (arc == 0) {
        arc = ble_hs_id_set_rnd(addr.val);
    }
    if (arc != 0) {
        /* No private address, no transmission.
         *
         * The first version of this ignored both return values, and then
         * asked the stack to advertise from BLE_OWN_ADDR_RANDOM anyway --
         * which failed with 21, BLE_HS_ENOADDR, every time. Silently: the
         * only clue was the advert never starting.
         *
         * Refusing is also the right behaviour rather than merely the safe
         * one. Falling back to the device's own public address would transmit
         * something that does not change, and an address that does not change
         * says "this same box was here yesterday" to anyone keeping a list.
         * That is the property the burst was designed around, so losing it is
         * a reason not to send, not a detail to carry on past. */
        ESP_LOGW(TAG, "no private address for a warning advert (%d); "
                      "not transmitting", arc);
        start_scan();
        return false;
    }

    int rc = ble_gap_adv_set_data(element, (int)n);
    if (rc != 0) {
        ESP_LOGW(TAG, "could not set the warning advert: %d", rc);
        start_scan();
        return false;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
        .itvl_min  = MS_TO_UNITS(WARN_ITVL_MIN_MS),
        .itvl_max  = MS_TO_UNITS(WARN_ITVL_MAX_MS),
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, WARN_BURST_MS,
                           &params, NULL, NULL);
    /* Listening resumes either way. A failed burst must not leave the device
     * deaf, which is a worse outcome than not having warned: the whole point
     * of the thing is receiving. */
    start_scan();
    if (rc != 0) {
        ESP_LOGW(TAG, "could not start the warning advert: %d", rc);
        return false;
    }
    return true;
}

bool observore_ble_can_warn(void) { return true; }

#else

/* Not compiled, not merely switched off. There is no runtime path from here
 * to an emission, which is the point of the Kconfig option selecting the
 * NimBLE broadcaster role rather than guarding a call. */
bool observore_ble_warn(const observore_peer_warning_t *w)
{
    (void)w;
    return false;
}

bool observore_ble_can_warn(void) { return false; }

#endif /* CONFIG_OBSERVORE_MESH_TX */

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
