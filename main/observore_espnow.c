#include "observore_espnow.h"

#include <string.h>

#if CONFIG_OBSERVORE_MESH_CENSUS

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/ccm.h"
#include "mbedtls/platform_util.h"
#include "soc/soc_caps.h"

#include "observore_census.h"
#include "observore_census_wire.h"
#include "observore_clock.h"
#include "observore_heapwatch.h"
#include "observore_meshkey.h"
#include "observore_nvs.h"
#include "observore_peer.h"

static const char *TAG = "observore.espnow";

/* The largest block of internal RAM a window needs before it may open: the
 * receive queue, the snapshot of this node's census it sends from, and what
 * ESP-NOW allocates for itself. The boards with no PSRAM run with an internal
 * heap whose floor has been measured in hundreds of bytes during a TLS session,
 * so a window that opened into that would take memory the notifier needs. Below
 * this the window is skipped and says so, never silently. */
#define OBSERVORE_CX_MIN_HEAP 8192

/* Frames the receive callback may hold for the main task. Eight is more than
 * one friend's whole census (eight chunks arrive in a burst) and few enough that
 * the queue costs two kilobytes, taken at open and returned at close. A full
 * queue drops the newest frame and counts it, because the callback may not
 * wait. */
#define OBSERVORE_CX_RXQ_LEN 8

/* Frames taken off that queue per pass of the main loop. The loop sleeps 200 ms
 * a pass while a window is open, so four a pass bounds what a flood of junk can
 * cost the main task: decrypting is cheap, but unbounded is not a number. */
#define OBSERVORE_CX_STEP_FRAMES 4

/* When this node first sends in a window, half a second in, so the radio and
 * the friends have settled and a node that opened a moment earlier is already
 * listening. */
#define OBSERVORE_CX_FIRST_ROUND_MS 500

/* The gap between rounds, and the last second a round may start in. A round is
 * every chunk of this node's census; repeating it every five seconds, plus or
 * minus one second of jitter from the hardware RNG, gives a friend whose window
 * opened late a second chance, and the jitter stops two nodes that started
 * together from colliding on every round. Twenty-six seconds leaves the last
 * round four seconds to be heard before the window closes at thirty. */
#define OBSERVORE_CX_ROUND_S      5
#define OBSERVORE_CX_LAST_ROUND_S 26

/* The labels the household key and its check are derived under. They are part
 * of the wire contract: a node that derived under another label would hear its
 * friends as noise. */
#define OBSERVORE_CX_KEY_LABEL   "observore/census/v1/key"
#define OBSERVORE_CX_CHECK_LABEL "observore/census/v1/check"

/* How long to wait for one send to complete before the next. ESP-IDF warns that
 * sending faster than the callback returns disorders the callback, so each frame
 * is sent after the last was done; at the default rate a frame is a few
 * milliseconds of air, so fifty is a ceiling and not a pace. */
#define OBSERVORE_CX_SEND_WAIT_MS 50

/* A broadcast: every node on the channel hears it, and ESP-NOW cannot encrypt a
 * broadcast, which is why the frame carries its own AES-CCM. */
static const uint8_t BROADCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* What the radio callback hands the main task: a length and the bytes. */
typedef struct {
    uint8_t len;
    uint8_t data[OBSERVORE_CX_FRAME_MAX];
} cx_item_t;

/* Everything below is static, so nothing sits on the main task's stack, which is
 * 6 KB and already carries the loop's depth. The CCM context, the frame buffer,
 * the plaintext buffer and the one queue item together are the working set of a
 * window, and they exist only in a build that has the exchange. */
static SemaphoreHandle_t s_cx_lock;          /* guards the state below, short holds only */
static bool              s_open;
static int64_t           s_opened_us;
static int64_t           s_last_close_us;
static int               s_day;              /* the census day this window runs on */
static uint16_t          s_window_id;
static bool              s_sharing;          /* exchange mode with something to say */
static bool              s_cut;              /* ended for a reason other than time */
static bool              s_send_logged;      /* a send error is said once a window */
static int64_t           s_next_round_us;

/* What the owner set, and the one fact that has to outlive a reboot with it:
 * the last local day a scheduled window ran, so a node that restarts during the
 * grace does not run the day's window twice.
 *
 * Eight bytes, written as they are and read back the same way, with a version so
 * that a later layout is recognised rather than guessed at. `reserved` is zero
 * and keeps the two sixteen-bit fields aligned. A last_day of 0xFFFF is "none":
 * 65535 is the last day the census can hold, and no window has run on it. */
typedef struct cx_settings_t {
    uint8_t  version;
    uint8_t  mode;
    uint8_t  channel;
    uint8_t  reserved;
    uint16_t at_min;
    uint16_t last_day;
} cx_settings_t;

_Static_assert(sizeof(cx_settings_t) == 8, "the stored exchange settings are eight bytes");

#define OBSERVORE_CX_SETTINGS_FORMAT 1
#define OBSERVORE_CX_NO_DAY          0xFFFF

/* The defaults are what a node has until the owner says otherwise: off, on
 * channel 1, at 03:30, with no scheduled window yet run. */
static cx_settings_t s_set = {
    .version  = OBSERVORE_CX_SETTINGS_FORMAT,
    .mode     = OBSERVORE_CX_OFF,
    .channel  = 1,
    .at_min   = OBSERVORE_CX_AT_DEFAULT,
    .last_day = OBSERVORE_CX_NO_DAY,
};
static bool s_manual;                        /* set when the owner asks for a window now */

static mbedtls_ccm_context s_ccm;
static uint8_t             s_frame[OBSERVORE_CX_FRAME_MAX];
static uint8_t             s_pt[OBSERVORE_CX_ENTRY_LEN * OBSERVORE_CX_ENTRIES_MAX];
static cx_item_t           s_item;

/* Taken at open and given back at close. The receive callback reads s_rxq once
 * into a local and returns if it is NULL, so a frame that arrives after close
 * began finds nothing to write to. */
static QueueHandle_t volatile s_rxq;
static SemaphoreHandle_t      s_txdone;

static observore_census_entry_t *s_send;     /* this node's census as of the window's day */
static size_t                    s_send_n;
static observore_cx_heard_t      s_heard;
static observore_cx_result_t     s_result;

/* Written by the receive callback alone and read only after ESP-NOW has been
 * deinitialised, when no callback can still be running. */
static volatile uint16_t s_rx_odd;           /* shorter than a header, or longer than a frame */
static volatile uint16_t s_rx_full;          /* the queue was full */

static observore_cx_decision_t s_last_skip = OBSERVORE_CX_WAIT;

#define CX_LOCK()   xSemaphoreTake(s_cx_lock, portMAX_DELAY)
#define CX_UNLOCK() xSemaphoreGive(s_cx_lock)

/* This node's id as its BLE warnings carry it, so a friend's console names this
 * node by the same four hex digits. Read once: the base MAC does not change. */
static uint16_t self_id(void)
{
    static uint16_t id;
    if (id == 0) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BT);
        id = observore_peer_node_id(mac);
    }
    return id;
}

/* The radio's receive callback, which runs on the Wi-Fi task. ESP-IDF says it
 * must not do lengthy work, and taking the census lock here would let a flash
 * commit on the main task stall the radio, so it copies and returns. It never
 * reads the receive info: the sender's address is not kept, and what a frame
 * says about its sender is read from the authenticated header or not at all. */
static void rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    (void)info;
    QueueHandle_t q = s_rxq;
    if (!q) {
        return;
    }
    if (data_len < OBSERVORE_CX_HDR_LEN || data_len > OBSERVORE_CX_FRAME_MAX) {
        s_rx_odd++;
        return;
    }
    cx_item_t item;
    item.len = (uint8_t)data_len;
    memcpy(item.data, data, (size_t)data_len);
    if (xQueueSend(q, &item, 0) != pdTRUE) {
        s_rx_full++;
    }
}

static void tx_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    (void)status;
    SemaphoreHandle_t s = s_txdone;
    if (s) {
        xSemaphoreGive(s);
    }
}

bool observore_espnow_available(void)
{
    return true;
}

/* Read the settings, which are the defaults when nothing is stored: the
 * exchange is off at first boot even in a build that has it, and the owner turns
 * it on. A blob of the wrong length, version or range is not guessed at either,
 * because a node that transmits on a schedule nobody chose is the failure this
 * option exists to avoid. */
static void settings_load(void)
{
    s_set.version  = OBSERVORE_CX_SETTINGS_FORMAT;
    s_set.mode     = OBSERVORE_CX_OFF;
    s_set.channel  = 1;
    s_set.reserved = 0;
    s_set.at_min   = OBSERVORE_CX_AT_DEFAULT;
    s_set.last_day = OBSERVORE_CX_NO_DAY;

    observore_nvs_item_t item = {.key = "cxsettings", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &s_set, .len = sizeof(s_set)};
    observore_nvs_read(&item, 1);
    if (!item.found) {
        return;
    }
    if (item.len != sizeof(s_set) || s_set.version != OBSERVORE_CX_SETTINGS_FORMAT ||
        s_set.mode > OBSERVORE_CX_EXCHANGE || !observore_cx_channel_ok(s_set.channel) ||
        s_set.at_min >= 1440) {
        s_set.version  = OBSERVORE_CX_SETTINGS_FORMAT;
        s_set.mode     = OBSERVORE_CX_OFF;
        s_set.channel  = 1;
        s_set.reserved = 0;
        s_set.at_min   = OBSERVORE_CX_AT_DEFAULT;
        s_set.last_day = OBSERVORE_CX_NO_DAY;
        ESP_LOGW(TAG, "the saved census exchange settings were not readable -- "
                      "the exchange is off");
    }
}

/* Write the settings. Called outside the lock, which it takes itself to copy
 * them, so a flash commit never holds up the receive path's readers. */
static void settings_save(void)
{
    cx_settings_t copy;
    CX_LOCK();
    copy = s_set;
    CX_UNLOCK();
    const observore_nvs_item_t item = {.key = "cxsettings", .type = OBSERVORE_NVS_BLOB,
                                       .buf = &copy, .len = sizeof(copy)};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the census exchange settings");
    }
}

void observore_espnow_init(void)
{
    s_cx_lock = xSemaphoreCreateMutex();
    mbedtls_ccm_init(&s_ccm);
    settings_load();

    /* After the household secret is loaded, and from it: the peer table is kept
     * only under the key check of the secret it was learned under. The check is
     * four bytes of an HMAC under its own label, so it names the secret without
     * being usable as a key, and the copy here is wiped once the table has been
     * read. */
    uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN] = {0};
    bool have_secret = observore_meshkey_derive(OBSERVORE_CX_CHECK_LABEL, check, sizeof(check));
    observore_census_peer_load(check, have_secret);
    mbedtls_platform_zeroize(check, sizeof(check));

    ESP_LOGI(TAG, "census exchange %s, window %02u:%02u, channel %u",
             observore_cx_mode_name(s_set.mode), (unsigned)(s_set.at_min / 60),
             (unsigned)(s_set.at_min % 60), (unsigned)s_set.channel);
}

/* Everything the decision to open reads, gathered here so the decision touches
 * no clock. The second of the local day is read only from a valid clock: a node
 * that has never been told the time would otherwise compute a "03:30" out of the
 * epoch. */
static void fill_when(observore_cx_when_t *w, int64_t now_us)
{
    memset(w, 0, sizeof(*w));
    w->clock_valid = observore_clock_valid();
    w->today = observore_clock_day();
    if (w->clock_valid) {
        time_t t = time(NULL);
        struct tm lt;
        localtime_r(&t, &lt);
        w->sod = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    }
    w->at_min        = s_set.at_min;
    w->last_day      = s_set.last_day == OBSERVORE_CX_NO_DAY ? -1 : (int)s_set.last_day;
    w->now_us        = now_us;
    w->last_close_us = s_last_close_us;
    w->mode          = s_set.mode;
    w->have_secret   = observore_meshkey_present();
}

bool observore_espnow_should_open(int64_t now_us)
{
    if (!s_cx_lock) {
        return false;
    }
    observore_cx_when_t w;
    bool save = false;
    CX_LOCK();
    fill_when(&w, now_us);
    w.manual = s_manual;
    observore_cx_decision_t d = observore_cx_decide(&w);
    if (d == OBSERVORE_CX_OPEN_NOW) {
        s_manual = false;
    } else if (d == OBSERVORE_CX_OPEN_SCHEDULED) {
        s_set.last_day = (uint16_t)w.today;
        save = true;
    } else if (w.manual && d != OBSERVORE_CX_SKIP_TOO_SOON) {
        /* A request the decision refused is a request answered: it is not held
         * over to fire the moment the reason clears. */
        s_manual = false;
    }
    bool say = d != s_last_skip;
    s_last_skip = d;
    CX_UNLOCK();

    /* Recorded in flash as well, so a reboot inside the grace does not run the
     * day's window a second time. */
    if (save) {
        settings_save();
    }
    if (say) {
        switch (d) {
        case OBSERVORE_CX_SKIP_NO_SECRET:
            ESP_LOGI(TAG, "census exchange waiting: no household secret");
            break;
        case OBSERVORE_CX_SKIP_NO_CLOCK:
            ESP_LOGI(TAG, "census exchange waiting: no clock");
            break;
        case OBSERVORE_CX_SKIP_TOO_SOON:
            ESP_LOGI(TAG, "census window refused: the last one closed under five minutes ago");
            break;
        default:
            break;
        }
    }
    return d == OBSERVORE_CX_OPEN_SCHEDULED || d == OBSERVORE_CX_OPEN_NOW;
}

bool observore_espnow_open(int64_t now_us, uint8_t left_mode)
{
    if (!s_cx_lock) {
        return false;
    }
    CX_LOCK();
    if (s_open) {
        CX_UNLOCK();
        return true;
    }

    /* Each step that has been done, so a failure undoes exactly those. */
    bool key_set = false, banded = false, queued = false;
    const char *why = NULL;
    esp_err_t err = ESP_OK;

    size_t largest = heap_caps_get_largest_free_block(OBSERVORE_HEAP_CAPS);
    if (largest < OBSERVORE_CX_MIN_HEAP) {
        ESP_LOGW(TAG, "census window skipped: low memory (largest free block %u bytes)",
                 (unsigned)largest);
        goto refused;
    }

    {
        uint8_t key[16];
        if (!observore_meshkey_derive(OBSERVORE_CX_KEY_LABEL, key, sizeof(key))) {
            ESP_LOGW(TAG, "census window refused: no key could be derived");
            goto refused;
        }
        mbedtls_ccm_init(&s_ccm);
        int rc = mbedtls_ccm_setkey(&s_ccm, MBEDTLS_CIPHER_ID_AES, key, 128);
        mbedtls_platform_zeroize(key, sizeof(key));
        if (rc != 0) {
            ESP_LOGW(TAG, "census window refused: the cipher would not take the key (%d)", rc);
            goto refused;
        }
        key_set = true;

        /* The secret may have changed since the peer table was saved, and a
         * window is the moment a table learned under the old one would start
         * to be believed. Emptied here, and written at close with the rest. */
        uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN] = {0};
        if (observore_meshkey_derive(OBSERVORE_CX_CHECK_LABEL, check, sizeof(check)) &&
            observore_census_peer_rekey(check)) {
            ESP_LOGW(TAG, "the household secret changed: the peer census was emptied");
        }
        mbedtls_platform_zeroize(check, sizeof(check));
    }

#if SOC_WIFI_SUPPORT_5G
    /* The household channel is a 2.4 GHz channel, and a chip that may scan 5 GHz
     * is pinned to the one band for the window. The patrol cycle sets the band
     * back to automatic every cycle; close does too, for the uplink. */
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (err != ESP_OK) {
        why = "the band could not be pinned to 2.4 GHz";
        goto failed;
    }
    banded = true;
#endif

    err = esp_wifi_set_channel(s_set.channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        why = "the household channel could not be set";
        goto failed;
    }

    s_rxq = xQueueCreate(OBSERVORE_CX_RXQ_LEN, sizeof(cx_item_t));
    s_txdone = xSemaphoreCreateBinary();
    s_send = heap_caps_malloc(sizeof(*s_send) * OBSERVORE_CENSUS_MAX, OBSERVORE_HEAP_CAPS);
    queued = true;
    if (!s_rxq || !s_txdone || !s_send) {
        why = "no memory for the receive queue";
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    s_rx_odd = 0;
    s_rx_full = 0;

    err = esp_now_init();
    if (err != ESP_OK) {
        why = "ESP-NOW would not start";
        goto failed;
    }
    err = esp_now_register_recv_cb(rx_cb);
    if (err == ESP_OK) {
        err = esp_now_register_send_cb(tx_cb);
    }
    if (err == ESP_OK) {
        /* Channel 0 is the current channel, which the window has just set. The
         * peer is the broadcast address, unencrypted: ESP-NOW cannot encrypt a
         * broadcast. */
        esp_now_peer_info_t bc = {0};
        memcpy(bc.peer_addr, BROADCAST, sizeof(bc.peer_addr));
        bc.channel = 0;
        bc.ifidx   = WIFI_IF_STA;
        bc.encrypt = false;
        err = esp_now_add_peer(&bc);
    }
    if (err != ESP_OK) {
        why = "ESP-NOW would not take its callbacks and peer";
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        goto failed;
    }

    s_day = observore_clock_day();
    s_window_id = (uint16_t)esp_random();
    memset(&s_heard, 0, sizeof(s_heard));
    memset(&s_result, 0, sizeof(s_result));
    s_result.mode      = s_set.mode;
    s_result.day       = s_day;
    s_result.left_mode = left_mode;
    s_send_n  = 0;
    s_sharing = false;
    if (s_set.mode == OBSERVORE_CX_EXCHANGE) {
        s_send_n  = observore_census_local_sendable(s_day, s_send, OBSERVORE_CENSUS_MAX);
        s_sharing = s_send_n > 0;
    }
    s_cut           = false;
    s_send_logged   = false;
    s_opened_us     = now_us;
    s_next_round_us = now_us + (int64_t)OBSERVORE_CX_FIRST_ROUND_MS * 1000;
    s_open          = true;
    ESP_LOGI(TAG, "census window open: %s on channel %u for up to %ds (%u entries to share)",
             s_sharing ? "share" : "listen", (unsigned)s_set.channel,
             OBSERVORE_CX_WINDOW_MAX_S, (unsigned)s_send_n);
    CX_UNLOCK();
    return true;

failed:
    ESP_LOGW(TAG, "census window refused: %s%s%s", why ? why : "failed",
             err != ESP_OK ? ": " : "", err != ESP_OK ? esp_err_to_name(err) : "");
    if (queued) {
        QueueHandle_t q = s_rxq;
        s_rxq = NULL;
        if (q) {
            vQueueDelete(q);
        }
        if (s_txdone) {
            vSemaphoreDelete(s_txdone);
            s_txdone = NULL;
        }
        free(s_send);
        s_send = NULL;
    }
#if SOC_WIFI_SUPPORT_5G
    if (banded) {
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    }
#else
    (void)banded;
#endif
refused:
    if (key_set) {
        mbedtls_ccm_free(&s_ccm);
    }
    CX_UNLOCK();
    return false;
}

/* One round: every chunk of this node's census, each sealed under a fresh nonce
 * and sent after the one before has been sent. Called from the main task only,
 * which is why the frame and plaintext buffers need no lock. A send error is
 * said once a window and ends the round, because a radio that refuses one frame
 * is not going to take the next seven. */
static void send_round(void)
{
    uint8_t chunks = observore_cx_plan(s_send_n);
    for (uint8_t i = 0; i < chunks; i++) {
        size_t first = 0;
        uint8_t count = 0;
        observore_cx_chunk(s_send_n, i, &first, &count);
        if (count == 0) {
            break;
        }
        observore_cx_hdr_t h = {
            .node = self_id(), .day = (uint16_t)s_day, .window = s_window_id,
            .index = i, .total = chunks, .count = count,
        };
        esp_fill_random(h.nonce, sizeof(h.nonce));
        observore_cx_put_hdr(&h, s_frame);
        for (uint8_t k = 0; k < count; k++) {
            observore_cx_put_entry(&s_send[first + k], s_pt + (size_t)k * OBSERVORE_CX_ENTRY_LEN);
        }
        size_t ct_len = (size_t)count * OBSERVORE_CX_ENTRY_LEN;
        int rc = mbedtls_ccm_encrypt_and_tag(&s_ccm, ct_len,
                                             s_frame + OBSERVORE_CX_NONCE_OFF,
                                             OBSERVORE_CX_NONCE_LEN,
                                             s_frame, OBSERVORE_CX_HDR_LEN,
                                             s_pt, s_frame + OBSERVORE_CX_HDR_LEN,
                                             s_frame + OBSERVORE_CX_HDR_LEN + ct_len,
                                             OBSERVORE_CX_TAG_LEN);
        if (rc != 0) {
            if (!s_send_logged) {
                s_send_logged = true;
                ESP_LOGW(TAG, "census frame could not be sealed (%d)", rc);
            }
            return;
        }
        /* A stale completion from the frame before must not read as this one's. */
        xSemaphoreTake(s_txdone, 0);
        esp_err_t err = esp_now_send(BROADCAST, s_frame,
                                     OBSERVORE_CX_HDR_LEN + ct_len + OBSERVORE_CX_TAG_LEN);
        if (err != ESP_OK) {
            if (!s_send_logged) {
                s_send_logged = true;
                ESP_LOGW(TAG, "census frame not sent: %s", esp_err_to_name(err));
            }
            return;
        }
        xSemaphoreTake(s_txdone, pdMS_TO_TICKS(OBSERVORE_CX_SEND_WAIT_MS));
    }
}

bool observore_espnow_step(int64_t now_us)
{
    if (!s_cx_lock) {
        return false;
    }
    CX_LOCK();
    if (!s_open) {
        CX_UNLOCK();
        return false;
    }
    if (observore_cx_should_close(s_opened_us, now_us)) {
        CX_UNLOCK();
        return false;
    }
    if (observore_clock_day() == OBSERVORE_CENSUS_NO_DAY || !observore_meshkey_present() ||
        (s_set.mode != OBSERVORE_CX_LISTEN && s_set.mode != OBSERVORE_CX_EXCHANGE)) {
        s_cut = true;
        CX_UNLOCK();
        return false;
    }

    for (int pass = 0; pass < OBSERVORE_CX_STEP_FRAMES; pass++) {
        if (xQueueReceive(s_rxq, &s_item, 0) != pdTRUE) {
            break;
        }
        observore_cx_hdr_t hdr;
        observore_cx_frame_t shape = observore_cx_check_frame(s_item.data, s_item.len, &hdr);
        if (shape == OBSERVORE_CX_FRAME_FOREIGN) {
            s_result.foreign++;
            continue;
        }
        if (shape != OBSERVORE_CX_FRAME_OK) {
            s_result.malformed++;
            continue;
        }

        /* The length the shape check has already tied to the entry count is all
         * that is used to open the frame: nothing the sender wrote is believed
         * until the tag has verified. */
        size_t ct_len = (size_t)s_item.len - OBSERVORE_CX_HDR_LEN - OBSERVORE_CX_TAG_LEN;
        int rc = mbedtls_ccm_auth_decrypt(&s_ccm, ct_len,
                                          s_item.data + OBSERVORE_CX_NONCE_OFF,
                                          OBSERVORE_CX_NONCE_LEN,
                                          s_item.data, OBSERVORE_CX_HDR_LEN,
                                          s_item.data + OBSERVORE_CX_HDR_LEN, s_pt,
                                          s_item.data + OBSERVORE_CX_HDR_LEN + ct_len,
                                          OBSERVORE_CX_TAG_LEN);
        if (rc != 0) {
            s_result.bad_tag++;       /* a number, and nothing else of the frame */
            continue;
        }

        /* Authentic. Only now are the sender's node, day, window and chunk
         * read. */
        if (hdr.node == self_id()) {
            continue;                 /* our own frame does not come back, but the check is free */
        }
        if (!observore_cx_day_ok(hdr.day, s_day)) {
            s_result.day_skew++;
            continue;
        }
        observore_cx_seen_t seen = observore_cx_heard_note(&s_heard, hdr.node, hdr.window,
                                                           hdr.index);
        if (seen == OBSERVORE_CX_DUPLICATE) {
            s_result.dropped++;
            continue;
        }
        if (seen == OBSERVORE_CX_CROWDED) {
            continue;                 /* counted by the heard table */
        }
        for (size_t i = 0; i < ct_len / OBSERVORE_CX_ENTRY_LEN; i++) {
            observore_census_entry_t e;
            if (!observore_cx_get_entry(s_pt + i * OBSERVORE_CX_ENTRY_LEN, hdr.day, &e)) {
                s_result.entries_bad++;
                continue;
            }
            observore_census_merge_t m = observore_census_peer_merge(hdr.node, s_day, &e);
            if (m == OBSERVORE_CENSUS_MERGE_CHANGED) {
                s_result.merged++;
            } else if (m == OBSERVORE_CENSUS_MERGE_REFUSED) {
                s_result.entries_bad++;
            }
        }
    }

    bool round_due = s_sharing && now_us >= s_next_round_us &&
                     now_us - s_opened_us < (int64_t)OBSERVORE_CX_LAST_ROUND_S * 1000000;
    if (round_due) {
        send_round();
        s_result.rounds++;
        s_result.sent = (uint16_t)(s_result.sent + s_send_n);
        int64_t jitter_ms = (int64_t)(esp_random() % 2001) - 1000;
        s_next_round_us = now_us + (int64_t)OBSERVORE_CX_ROUND_S * 1000000 + jitter_ms * 1000;
    }
    CX_UNLOCK();
    return true;
}

void observore_espnow_close(int64_t now_us, bool interrupted)
{
    if (!s_cx_lock) {
        return;
    }
    CX_LOCK();
    if (!s_open) {
        CX_UNLOCK();
        return;
    }

    /* Before anything else touches the radio: stopping Wi-Fi with ESP-NOW up is
     * how a driver asserts instead of returning an error. */
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    QueueHandle_t q = s_rxq;
    s_rxq = NULL;
    if (q) {
        vQueueDelete(q);
    }
    if (s_txdone) {
        vSemaphoreDelete(s_txdone);
        s_txdone = NULL;
    }
    free(s_send);
    s_send = NULL;
    mbedtls_ccm_free(&s_ccm);
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    int64_t secs = (now_us - s_opened_us) / 1000000;
    if (secs < 0) {
        secs = 0;
    }
    if (secs > OBSERVORE_CX_WINDOW_MAX_S) {
        secs = OBSERVORE_CX_WINDOW_MAX_S;
    }
    s_result.secs        = (uint8_t)secs;
    s_result.interrupted = interrupted || s_cut;
    s_result.friends     = s_heard.n;
    for (uint8_t i = 0; i < s_heard.n && i < OBSERVORE_CX_FRIENDS_MAX; i++) {
        s_result.friend_node[i] = s_heard.f[i].node;
    }
    s_result.foreign += s_rx_odd;
    s_result.dropped += s_rx_full;
    s_result.crowded  = s_heard.crowded;
    s_result.closed_us = now_us;
    s_last_close_us = now_us;
    s_open = false;

    observore_census_peer_prune(s_day);
    /* At most one write a window, and only when the table changed. */
    if (observore_census_peer_take_changed()) {
        observore_census_peer_save();
    }

    static char text[OBSERVORE_CX_RESULT_TEXT_LEN];
    observore_cx_result_text(&s_result, text, sizeof(text));
    ESP_LOGI(TAG, "census window closed: %s", text);
    ESP_LOGI(TAG, "main task stack: %u bytes unused after the census window",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    CX_UNLOCK();
}

bool observore_espnow_is_open(void)
{
    if (!s_cx_lock) {
        return false;
    }
    CX_LOCK();
    bool open = s_open;
    CX_UNLOCK();
    return open;
}

bool observore_espnow_soon(int64_t now_us, int lead_s)
{
    if (!s_cx_lock) {
        return false;
    }
    observore_cx_when_t w;
    CX_LOCK();
    fill_when(&w, now_us);
    CX_UNLOCK();
    long left = observore_cx_seconds_until(&w);
    return left >= 0 && left <= lead_s;
}

_Static_assert(OBSERVORE_ESPNOW_FRIENDS == OBSERVORE_CX_FRIENDS_MAX,
               "the console's friend list is the exchange's");
_Static_assert(OBSERVORE_ESPNOW_TEXT_LEN == OBSERVORE_CX_RESULT_TEXT_LEN,
               "the console's result text is the exchange's");

/* What the console reads. Each field of the settings and of the last result is
 * copied under the exchange lock, and the census is asked for its own numbers
 * afterwards, outside it, so the order of locks stays exchange then census and
 * never the other way round. */
void observore_espnow_status(int64_t now_us, observore_espnow_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->available = true;
    out->blocked   = "";
    out->next_s    = -1;
    if (!s_cx_lock) {
        return;
    }

    observore_cx_when_t w;
    observore_cx_result_t r;
    CX_LOCK();
    fill_when(&w, now_us);
    out->mode   = s_set.mode;
    out->at_min = s_set.at_min;
    out->channel = s_set.channel;
    r = s_result;
    CX_UNLOCK();

    out->secret = w.have_secret;
    out->next_s = observore_cx_seconds_until(&w);
    switch (observore_cx_decide(&w)) {
    case OBSERVORE_CX_SKIP_OFF:
        out->blocked = "off";
        break;
    case OBSERVORE_CX_SKIP_NO_SECRET:
        out->blocked = "no household secret";
        break;
    case OBSERVORE_CX_SKIP_NO_CLOCK:
        out->blocked = "no clock";
        break;
    default:
        break;
    }

    out->peer_entries = (unsigned)observore_census_peer_count();
    out->peer_max     = OBSERVORE_PEER_CENSUS_MAX;
    out->forgot       = (uint8_t)observore_census_peer_forgot();

    /* closed_us is zero until the first window of this boot has closed. */
    if (r.closed_us != 0) {
        out->have_last        = true;
        out->last_share       = r.mode == OBSERVORE_CX_EXCHANGE;
        out->last_interrupted = r.interrupted;
        out->last_secs        = r.secs;
        out->last_left        = r.left_mode;
        out->last_sent        = r.sent;
        out->last_friends     = r.friends > OBSERVORE_CX_FRIENDS_MAX ? OBSERVORE_CX_FRIENDS_MAX
                                                                      : r.friends;
        for (uint8_t i = 0; i < out->last_friends; i++) {
            out->last_friend[i] = r.friend_node[i];
        }
        out->last_merged    = r.merged;
        out->last_bad_tag   = r.bad_tag;
        out->last_day_skew  = r.day_skew;
        out->last_malformed = r.malformed;
        out->last_foreign   = r.foreign;
        out->last_closed_us = r.closed_us;
        observore_cx_result_text(&r, out->last_text, sizeof(out->last_text));
    }
}

/* Each given value is parsed before anything is applied, so a refusal leaves the
 * settings as they were. A mode other than off needs a household secret: the key
 * the frames are sealed under comes from it, and an exchange that could not be
 * keyed would be a node transmitting or listening for nothing. */
const char *observore_espnow_configure(const char *mode, const char *at, const char *channel)
{
    uint8_t  new_mode = 0;
    uint16_t new_at = 0;
    int      new_chan = 0;

    if (mode && !observore_cx_mode_parse(mode, &new_mode)) {
        return "mode is off, listen or exchange";
    }
    if (at && !observore_cx_at_parse(at, &new_at)) {
        return "time is HH:MM, 00:00 to 23:59";
    }
    if (channel) {
        char *end = NULL;
        long v = strtol(channel, &end, 10);
        if (end == channel || *end != '\0' || !observore_cx_channel_ok((int)v)) {
            return "channel is 1 to 13";
        }
        new_chan = (int)v;
    }
    if (mode && new_mode != OBSERVORE_CX_OFF && !observore_meshkey_present()) {
        return "set a household secret first";
    }
    if (!s_cx_lock) {
        return "the census exchange is not ready yet";
    }

    CX_LOCK();
    if (mode) {
        s_set.mode = new_mode;
    }
    if (at) {
        s_set.at_min = new_at;
    }
    if (channel) {
        s_set.channel = (uint8_t)new_chan;
    }
    uint8_t m = s_set.mode, ch = s_set.channel;
    unsigned a = s_set.at_min;
    CX_UNLOCK();

    settings_save();
    ESP_LOGI(TAG, "census exchange set to %s, window %02u:%02u, channel %u",
             observore_cx_mode_name(m), a / 60, a % 60, (unsigned)ch);
    return NULL;
}

/* A flag and nothing more. The decision is the one the schedule uses, run with
 * the request marked, so the reasons a hand-opened window is refused are the
 * reasons the main loop would refuse it: off, no secret, no clock, and a gap of
 * under five minutes since the last one. */
const char *observore_espnow_request_now(int64_t now_us)
{
    if (!s_cx_lock) {
        return "the census exchange is not ready yet";
    }
    observore_cx_when_t w;
    const char *why = NULL;
    CX_LOCK();
    if (s_open) {
        why = "a census window is open now";
    } else {
        fill_when(&w, now_us);
        w.manual = true;
        switch (observore_cx_decide(&w)) {
        case OBSERVORE_CX_OPEN_NOW:
            s_manual = true;
            break;
        case OBSERVORE_CX_SKIP_OFF:
            why = "the exchange is off";
            break;
        case OBSERVORE_CX_SKIP_NO_SECRET:
            why = "set a household secret first";
            break;
        case OBSERVORE_CX_SKIP_NO_CLOCK:
            why = "the clock is not set: no window can be dated";
            break;
        default:
            why = "the last window closed under five minutes ago";
            break;
        }
    }
    CX_UNLOCK();
    if (!why) {
        ESP_LOGI(TAG, "census window requested from the console");
    }
    return why;
}

const char *observore_espnow_forget(void)
{
    observore_census_peer_forget(OBSERVORE_CENSUS_FORGOT_OWNER);
    observore_census_peer_save();
    ESP_LOGI(TAG, "peer census forgotten by the owner; this node's own census is kept");
    return NULL;
}

/* Called after the console set or cleared the household secret. A window would
 * notice at its next open, but the console reads the table before then, and a
 * table kept under a secret that is gone would sit there saying nothing about
 * why it is no longer believed. */
void observore_espnow_secret_changed(void)
{
    uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN] = {0};
    if (observore_meshkey_derive(OBSERVORE_CX_CHECK_LABEL, check, sizeof(check))) {
        observore_census_peer_rekey(check);
    } else {
        observore_census_peer_forget(OBSERVORE_CENSUS_FORGOT_NO_SECRET);
    }
    mbedtls_platform_zeroize(check, sizeof(check));

    if (observore_census_peer_take_changed()) {
        observore_census_peer_save();
        ESP_LOGW(TAG, "the household secret %s: the peer census was emptied",
                 observore_meshkey_present() ? "changed" : "was cleared");
    }
}

#else /* !CONFIG_OBSERVORE_MESH_CENSUS */

bool observore_espnow_available(void)
{
    return false;
}

void observore_espnow_init(void)
{
}

void observore_espnow_status(int64_t now_us, observore_espnow_status_t *out)
{
    (void)now_us;
    memset(out, 0, sizeof(*out));
    out->blocked = "";
    out->next_s  = -1;
}

const char *observore_espnow_configure(const char *mode, const char *at, const char *channel)
{
    (void)mode;
    (void)at;
    (void)channel;
    return "the census exchange is not in this build";
}

const char *observore_espnow_request_now(int64_t now_us)
{
    (void)now_us;
    return "the census exchange is not in this build";
}

const char *observore_espnow_forget(void)
{
    return "the census exchange is not in this build";
}

void observore_espnow_secret_changed(void)
{
}

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
