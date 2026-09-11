#include <string.h>

#include "argus_track.h"
#include "argus_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "argus.wifi";

#define ARGUS_SCAN_MS      3000
#define ARGUS_SNIFF_MS     5000
#define ARGUS_CHANNEL_MIN  1
#define ARGUS_CHANNEL_MAX  13
#define ARGUS_MAX_AP       32

/* ASTM F3411 Remote ID over Wi-Fi: a vendor-specific IE (element 0xDD) whose
 * OUI is FA:0B:BC with vendor type 0x0D. */
static const uint8_t ASTM_OUI[3]     = {0xFA, 0x0B, 0xBC};
#define ASTM_VENDOR_TYPE_ODID 0x0D
#define IE_VENDOR_SPECIFIC    0xDD
#define IE_SSID               0x00

static argus_mode_t       s_mode = ARGUS_MODE_PATROL;
static esp_netif_t       *s_ap_netif;
static char               s_ap_ssid[32];
static bool               s_initialised;
/* s_mode is only meaningful once it has been pushed into the driver.  Without
 * this, the first set_mode(PATROL) matches the initial value of s_mode, takes
 * the no-op path, and esp_wifi_start() never runs -- leaving the sniffer
 * working but every AP scan failing with ESP_ERR_WIFI_NOT_STARTED. */
static bool               s_mode_applied;
static uint32_t           s_sniffed_frames;   /* accepted by the parser */
static uint32_t           s_sniffer_calls;    /* raw callback entries */

/* ------------------------------------------------------------------ */
/* Promiscuous sniffing                                               */
/* ------------------------------------------------------------------ */

/* 802.11 management frame header, enough of it to reach the fixed parameters
 * that precede the tagged IEs. */
typedef struct __attribute__((packed)) {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1[6];   /* destination */
    uint8_t  addr2[6];   /* source / transmitter */
    uint8_t  addr3[6];   /* BSSID */
    uint16_t seq_ctrl;
} wifi_mgmt_hdr_t;

/* Beacon and probe-response bodies both open with 12 bytes of fixed
 * parameters (timestamp, beacon interval, capability) before the IEs. */
#define MGMT_FIXED_PARAMS_LEN 12

static uint8_t frame_subtype(uint16_t fc)
{
    return (uint8_t)((fc >> 4) & 0x0F);
}

static uint8_t frame_type(uint16_t fc)
{
    return (uint8_t)((fc >> 2) & 0x03);
}

/* Walk the tagged IEs looking for the SSID and for an ASTM Remote ID element.
 * Returns true when a Remote ID element was present. */
static bool parse_ies(const uint8_t *ies, size_t len, char *ssid, size_t ssid_len)
{
    bool odid = false;
    size_t i = 0;

    while (i + 2 <= len) {
        uint8_t id = ies[i];
        uint8_t ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) {
            break;  /* truncated capture -- stop rather than read past the end */
        }
        const uint8_t *body = &ies[i + 2];

        if (id == IE_SSID && ssid && ssid_len) {
            size_t n = ie_len < ssid_len - 1 ? ie_len : ssid_len - 1;
            for (size_t k = 0; k < n; k++) {
                /* SSIDs are attacker-controlled bytes; keep only printable
                 * ASCII so nothing downstream has to defend itself. */
                ssid[k] = (body[k] >= 0x20 && body[k] < 0x7F) ? (char)body[k] : '.';
            }
            ssid[n] = '\0';
        } else if (id == IE_VENDOR_SPECIFIC && ie_len >= 4 &&
                   memcmp(body, ASTM_OUI, 3) == 0 &&
                   body[3] == ASTM_VENDOR_TYPE_ODID) {
            odid = true;
        }

        i += 2 + ie_len;
    }
    return odid;
}

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    s_sniffer_calls++;
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    /* rx_ctrl.sig_len includes the 4-byte FCS the radio appends. */
    if (pkt->rx_ctrl.sig_len < sizeof(wifi_mgmt_hdr_t) + MGMT_FIXED_PARAMS_LEN + 4) {
        return;
    }

    const wifi_mgmt_hdr_t *hdr = (const wifi_mgmt_hdr_t *)pkt->payload;
    if (frame_type(hdr->frame_control) != 0) {
        return;  /* not a management frame */
    }
    uint8_t subtype = frame_subtype(hdr->frame_control);
    /* 8 = beacon, 5 = probe response.  Remote ID rides on beacons; SSIDs are
     * worth harvesting from both. */
    if (subtype != 8 && subtype != 5) {
        return;
    }

    size_t hdr_len = sizeof(wifi_mgmt_hdr_t) + MGMT_FIXED_PARAMS_LEN;
    size_t ie_len = pkt->rx_ctrl.sig_len - hdr_len - 4;
    const uint8_t *ies = pkt->payload + hdr_len;

    char ssid[33] = {0};
    bool odid = parse_ies(ies, ie_len, ssid, sizeof(ssid));

    argus_observation_t obs = {
        .mac       = hdr->addr2,
        .src       = ARGUS_SRC_WIFI_SNIFF,
        .rssi      = (int8_t)pkt->rx_ctrl.rssi,
        .channel   = pkt->rx_ctrl.channel,
        .ssid      = ssid[0] ? ssid : NULL,
        .remote_id = odid,
    };
    s_sniffed_frames++;
    argus_track_observe(&obs, esp_timer_get_time());
}

/* ------------------------------------------------------------------ */
/* Active AP scan                                                     */
/* ------------------------------------------------------------------ */

static void run_ap_scan(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,           /* all channels */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* scan_time is deliberately left at zero.  With Bluetooth enabled the
         * driver rejects custom active-scan timing outright ("Should use
         * default active scan time parameter") and the scan returns nothing,
         * so the coexistence arbiter picks the dwell instead. */
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed in %s mode: %s",
                 s_mode == ARGUS_MODE_PATROL ? "patrol" : "console",
                 esp_err_to_name(err));
        return;
    }

    uint16_t count = ARGUS_MAX_AP;
    static wifi_ap_record_t records[ARGUS_MAX_AP];
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan results unavailable: %s", esp_err_to_name(err));
        return;
    }

    int64_t now = esp_timer_get_time();
    for (uint16_t i = 0; i < count; i++) {
        argus_observation_t obs = {
            .mac     = records[i].bssid,
            .src     = ARGUS_SRC_WIFI_SCAN,
            .rssi    = records[i].rssi,
            .channel = records[i].primary,
            .ssid    = (const char *)records[i].ssid,
        };
        argus_track_observe(&obs, now);
    }
    ESP_LOGI(TAG, "scan: %u APs", count);
}

/* ------------------------------------------------------------------ */
/* Mode handling                                                      */
/* ------------------------------------------------------------------ */

static void derive_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* The SSID carries the last three MAC bytes so two units in the same room
     * are distinguishable, and deliberately does not say "argus". */
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), CONFIG_ARGUS_AP_SSID_PREFIX "-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

uint32_t argus_wifi_sniffed_frames(void)
{
    return s_sniffed_frames;
}

uint32_t argus_wifi_sniffer_calls(void)
{
    return s_sniffer_calls;
}

const char *argus_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *argus_wifi_ap_password(void)
{
    return CONFIG_ARGUS_AP_PASSWORD;
}

esp_err_t argus_wifi_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    /* Nothing here needs to survive a reboot, and writing the Wi-Fi config to
     * flash on every mode change would wear it for no reason. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    derive_ap_ssid();
    s_initialised = true;

    return argus_wifi_set_mode(ARGUS_MODE_PATROL);
}

argus_mode_t argus_wifi_mode(void)
{
    return s_mode;
}

esp_err_t argus_wifi_set_mode(argus_mode_t mode)
{
    if (s_mode_applied && mode == s_mode) {
        return ESP_OK;
    }

    /* Always leave promiscuous mode before touching the interface config --
     * changing mode underneath an active sniffer is how you get a driver
     * assert instead of an error code. */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();

    if (mode == ARGUS_MODE_CONSOLE) {
        wifi_config_t ap = {0};
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s",
                 CONFIG_ARGUS_AP_PASSWORD);
        ap.ap.channel = 6;
        ap.ap.max_connection = 2;
        ap.ap.authmode = strlen(CONFIG_ARGUS_AP_PASSWORD) >= 8
                             ? WIFI_AUTH_WPA2_PSK
                             : WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "console mode: SSID \"%s\" at 192.168.4.1", s_ap_ssid);
    } else {
        /* Station mode but never associated: that is what leaves the radio
         * free to be retuned to any channel. */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        /* Power save must be off to sniff.  The default WIFI_PS_MIN_MODEM
         * sleeps the receiver between beacons, which on an unassociated
         * station means it is asleep for nearly all of the sweep. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_LOGI(TAG, "patrol mode: scanning and sniffing");
    }

    s_mode = mode;
    s_mode_applied = true;
    return ESP_OK;
}

void argus_wifi_patrol_cycle(void)
{
    if (s_mode != ARGUS_MODE_PATROL) {
        return;
    }

    run_ap_scan();

    /* Sniff sweep.  Dwell is split evenly across the channels; a shorter dwell
     * covers the band faster but a beacon interval is typically ~102 ms, so
     * anything under about 120 ms per channel starts missing APs outright. */
    const int channels = ARGUS_CHANNEL_MAX - ARGUS_CHANNEL_MIN + 1;
    const int dwell_ms = ARGUS_SNIFF_MS / channels;

    /* Register the filter and callback here, immediately before enabling
     * promiscuous mode, rather than once at init.  Registering them on a
     * stopped driver silently does not survive the esp_wifi_stop()/start()
     * that a mode change performs, and the symptom is a sniffer that reports
     * ic_enable_sniffer and then never delivers a single frame.
     *
     * The filter drops everything but management frames in the driver: data
     * frames are the overwhelming majority of the air, and rejecting them
     * before the callback is what keeps this cheap. */
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    for (int ch = ARGUS_CHANNEL_MIN; ch <= ARGUS_CHANNEL_MAX; ch++) {
        if (s_mode != ARGUS_MODE_PATROL) {
            break;  /* a mode switch landed mid-sweep */
        }
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    }
    esp_wifi_set_promiscuous(false);
}
