#include <string.h>

#include "observore_netcfg.h"
#include "mdns.h"
#include "observore_track.h"
#include "observore_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "observore.wifi";

/* Runtime transitions report failure; they do not abort.
 *
 * ESP_ERROR_CHECK reboots, and these run every time the device moves between
 * patrol and uplink -- roughly every two and a half minutes, for as long as it
 * is deployed. A transient driver error would therefore not degrade the
 * device, it would restart it, and because the device table and the follower
 * heuristic live in RAM, a restart throws away exactly the accumulating
 * evidence the device exists to gather. Staying in the current mode and
 * retrying on the next window is strictly better.
 *
 * Initialisation keeps ESP_ERROR_CHECK: if the radio will not come up at all
 * there is nothing useful to continue doing. */
#define TRY(expr, what)                                                     \
    do {                                                                    \
        esp_err_t _err = (expr);                                            \
        if (_err != ESP_OK) {                                               \
            ESP_LOGW(TAG, "%s failed: %s -- staying put and retrying",      \
                     (what), esp_err_to_name(_err));                        \
            return _err;                                                    \
        }                                                                   \
    } while (0)

#define OBSERVORE_SNIFF_MS     5000
#if SOC_WIFI_SUPPORT_5G
/* A dual-band scan returns both bands at once.  Measured in an ordinary flat,
 * that is 29 access points where the 2.4 GHz-only count was 13 -- already 29 of
 * 32 records, one neighbour away from silently dropping the rest.  The failure
 * would be invisible: a truncated list looks exactly like a quiet street. */
#define OBSERVORE_MAX_AP       64
#else
#define OBSERVORE_MAX_AP       32
#endif

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The sniffer sweeps a list rather than a range, because 5 GHz channel numbers
 * are not contiguous.  esp_wifi_set_channel() moves the radio across bands by
 * itself -- Espressif recommends it over esp_wifi_set_band() -- so one table of
 * channel numbers is enough to drive a dual-band sweep. */
static const uint8_t CHANNELS_2G[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

#if SOC_WIFI_SUPPORT_5G
/* UNII-1, UNII-2A, UNII-2C and UNII-3.  DFS channels are included: the radar
 * obligations that come with them apply to transmitting, and this radio only
 * ever listens.  Channels the configured country forbids are skipped at
 * runtime rather than pruned here, because the regulatory domain is not known
 * at compile time. */
static const uint8_t CHANNELS_5G[] = {
     36,  40,  44,  48,
     52,  56,  60,  64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};

/* 5 GHz is swept a slice at a time.  Covering all twenty-five channels in one
 * cycle would push the per-channel dwell under a beacon interval (~102 ms),
 * and missing beacons outright costs more than a slower rotation does: this
 * way 2.4 GHz stays fully covered every cycle and 5 GHz comes round in five.
 * The cursor persists across cycles, so the slice advances each sweep. */
#define OBSERVORE_SLICE_5G 5
static size_t s_5g_cursor;
#else
#define OBSERVORE_SLICE_5G 0
#endif

/* ASTM F3411 Remote ID over Wi-Fi: a vendor-specific IE (element 0xDD) whose
 * OUI is FA:0B:BC with vendor type 0x0D. */
static const uint8_t ASTM_OUI[3]     = {0xFA, 0x0B, 0xBC};
#define ASTM_VENDOR_TYPE_ODID 0x0D
#define IE_VENDOR_SPECIFIC    0xDD
#define IE_SSID               0x00

static observore_scan_entry_t s_last_scan[OBSERVORE_SCAN_REPORT_MAX];
static size_t                 s_last_scan_count;

static observore_mode_t       s_mode = OBSERVORE_MODE_PATROL;
static char               s_ap_ssid[32];
static char               s_hostname[48];
static esp_netif_t       *s_sta_netif;
static bool               s_initialised;
/* s_mode is only meaningful once it has been pushed into the driver.  Without
 * this, the first set_mode(PATROL) matches the initial value of s_mode, takes
 * the no-op path, and esp_wifi_start() never runs -- leaving the sniffer
 * working but every AP scan failing with ESP_ERR_WIFI_NOT_STARTED. */
static bool               s_mode_applied;
static uint32_t           s_sniffed_frames;
static EventGroupHandle_t s_sta_events;
static char               s_uplink_ip[16];
static char               s_uplink_error[160];
static bool               s_uplink_connected;
static int                s_connect_attempts;

#define STA_BIT_GOT_IP  BIT0
#define STA_BIT_FAILED  BIT1
/* Retries within one connect attempt.  A handful covers a slow AP or a
 * transient DHCP failure; beyond that the credentials are wrong or the network
 * is out of range, and sitting here retrying costs detection time. */
#define STA_MAX_RETRY   4   /* accepted by the parser */
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

    observore_observation_t obs = {
        .mac       = hdr->addr2,
        .src       = OBSERVORE_SRC_WIFI_SNIFF,
        .rssi      = (int8_t)pkt->rx_ctrl.rssi,
        .channel   = pkt->rx_ctrl.channel,
        .ssid      = ssid[0] ? ssid : NULL,
        .remote_id = odid,
    };
    s_sniffed_frames++;
    observore_track_observe(&obs, esp_timer_get_time());
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
        /* PASSIVE, and that is the whole point.
         *
         * An active scan broadcasts probe requests carrying this device's MAC
         * on every channel, every patrol cycle.  Probe requests are exactly
         * what presence analytics and Wi-Fi tracking systems collect -- so a
         * detector built to notice trackers was announcing itself to them
         * every couple of minutes, while claiming to listen without answering.
         *
         * Passive scanning waits for beacons instead.  Access points beacon
         * about ten times a second, including hidden ones (with a blank SSID),
         * so the cost is dwell time rather than coverage.
         *
         * scan_time is left at zero: with Bluetooth enabled the driver rejects
         * custom scan timing and returns nothing, so the coexistence arbiter
         * picks the dwell. */
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* block */);
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGD(TAG, "scan skipped, radio busy");  /* transient, self-clears */
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed in %s mode: %s", observore_mode_name(s_mode),
                 esp_err_to_name(err));
        return;
    }

    uint16_t count = OBSERVORE_MAX_AP;
    static wifi_ap_record_t records[OBSERVORE_MAX_AP];
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan results unavailable: %s", esp_err_to_name(err));
        return;
    }

    /* Keep a copy for Improv, which needs SSIDs rather than the tracker's
     * view.  Named networks only: a hidden AP is a beacon with a blank SSID
     * and cannot be offered as something to join. */
    size_t kept = 0;
    for (uint16_t i = 0; i < count && kept < OBSERVORE_SCAN_REPORT_MAX; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        snprintf(s_last_scan[kept].ssid, sizeof(s_last_scan[kept].ssid), "%s",
                 (const char *)records[i].ssid);
        s_last_scan[kept].rssi   = records[i].rssi;
        s_last_scan[kept].secure = records[i].authmode != WIFI_AUTH_OPEN;
        kept++;
    }
    s_last_scan_count = kept;

    int64_t now = esp_timer_get_time();
    for (uint16_t i = 0; i < count; i++) {
        observore_observation_t obs = {
            .mac     = records[i].bssid,
            .src     = OBSERVORE_SRC_WIFI_SCAN,
            .rssi    = records[i].rssi,
            .channel = records[i].primary,
            .ssid    = (const char *)records[i].ssid,
        };
        observore_track_observe(&obs, now);
    }
    /* Report the band split when there is one.  On a dual-band chip this is
     * the only cheap confirmation that 5 GHz is actually being swept rather
     * than silently refused by the regulatory domain, and on a 2.4 GHz-only
     * chip the extra clause never appears. */
    uint16_t on_5g = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (records[i].primary > 14) {
            on_5g++;
        }
    }
    if (on_5g) {
        ESP_LOGI(TAG, "scan: %u APs (%u on 2.4 GHz, %u on 5 GHz)",
                 count, (unsigned)(count - on_5g), (unsigned)on_5g);
    } else {
        ESP_LOGI(TAG, "scan: %u APs", count);
    }
    if (count >= OBSERVORE_MAX_AP) {
        ESP_LOGW(TAG, "scan filled the %d-record buffer; some APs were dropped",
                 OBSERVORE_MAX_AP);
    }
}

/* ------------------------------------------------------------------ */
/* Station uplink                                                     */
/* ------------------------------------------------------------------ */

/* The codes worth translating.  A bare number sends people to a search engine
 * when the answer is usually "wrong password" or "wrong band". */
static const char *wifi_reason_text(int reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "authentication rejected -- wrong password?";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "handshake failed -- almost always a wrong password";
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found -- check the SSID, and that it is "
                   "2.4 GHz (the ESP32-S3 has no 5 GHz radio)";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            /* Observed cause, in practice, was an empty stored password being
             * offered to a WPA2 network -- so lead with that rather than with
             * the exotic explanation. */
            return "security mismatch -- is the password set? an empty one "
                   "asks for an open network, which a WPA2 AP refuses";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "found, but its auth mode was rejected";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "found, but too weak";
        case WIFI_REASON_ASSOC_FAIL:
            return "association refused -- MAC filtering?";
        case WIFI_REASON_ASSOC_TOOMANY:
            return "the access point is full";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "lost the access point";
        case WIFI_REASON_CONNECTION_FAIL:
            return "connection failed";
        case WIFI_REASON_STA_LEAVING:
            return "we disconnected";
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_AUTH_LEAVE:
            /* Seen transiently when the radio has just been reconfigured out
             * of SoftAP mode; the next attempt normally succeeds. */
            return "the access point dropped the association";
        default:
            return "see WIFI_REASON_* in esp_wifi_types";
    }
}

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;

        /* Log EVERY attempt, not just the last.  Logging only the final one
         * reported reason 36 -- our own disconnect in the timeout path --
         * which says nothing about why the join actually failed. */
        s_uplink_connected = false;
        if (e->reason != WIFI_REASON_STA_LEAVING) {
            snprintf(s_uplink_error, sizeof(s_uplink_error), "%s (reason %d)",
                     wifi_reason_text(e->reason), e->reason);
            ESP_LOGW(TAG, "uplink attempt %d/%d failed: %s",
                     s_connect_attempts + 1, STA_MAX_RETRY + 1, s_uplink_error);
        }

        if (s_connect_attempts < STA_MAX_RETRY) {
            s_connect_attempts++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_sta_events, STA_BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_uplink_ip, sizeof(s_uplink_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_uplink_connected = true;
        s_uplink_error[0] = '\0';
        /* Reset the retry budget on success, so a later drop gets a fresh set
         * of attempts instead of inheriting an exhausted counter and never
         * reconnecting. */
        s_connect_attempts = 0;
        ESP_LOGI(TAG, "uplink up at %s", s_uplink_ip);
        xEventGroupSetBits(s_sta_events, STA_BIT_GOT_IP);
    }
}

const char *observore_wifi_uplink_ip(void)
{
    return s_uplink_ip;
}

const char *observore_wifi_uplink_error(void)
{
    return s_uplink_error;
}

bool observore_wifi_uplink_connected(void)
{
    return s_uplink_connected && s_mode == OBSERVORE_MODE_UPLINK;
}

esp_err_t observore_wifi_uplink_connect(void)
{
    observore_netcfg_t cfg;
    if (!observore_netcfg_get(&cfg)) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();

    /* The driver's fields are fixed-size and length-delimited, not strings, so
     * they are filled by length.  snprintf would both warn about truncation
     * and waste a byte on a terminator the driver does not want. */
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, cfg.ssid,
           strnlen(cfg.ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, cfg.password,
           strnlen(cfg.password, sizeof(sta.sta.password)));

    /* Accept whatever security the network offers.
     *
     * Left at defaults, a WPA2/WPA3 transition network or one that requires
     * protected management frames is rejected before any credential is even
     * tried, with reason 210 (NO_AP_FOUND_W_COMPATIBLE_SECURITY) -- which
     * reads like "wrong network" when the network is right there.
     *
     * threshold.authmode OPEN means "do not refuse on auth mode alone";
     * pmf capable-but-not-required joins both PMF and non-PMF networks; and
     * BOTH lets SAE negotiate either hunting-and-pecking or hash-to-element,
     * since access points differ on which they offer. */
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    /* Wipe the copy on our stack as soon as the driver has it. */
    memset(&cfg, 0, sizeof(cfg));

    s_uplink_ip[0] = '\0';
    s_uplink_error[0] = '\0';
    s_uplink_connected = false;
    s_connect_attempts = 0;
    xEventGroupClearBits(s_sta_events, STA_BIT_GOT_IP | STA_BIT_FAILED);

    TRY(esp_wifi_set_mode(WIFI_MODE_STA), "station mode");
    TRY(esp_wifi_set_config(WIFI_IF_STA, &sta), "station config");
    memset(&sta, 0, sizeof(sta));
    TRY(esp_wifi_start(), "wifi start");
    TRY(esp_wifi_set_ps(WIFI_PS_NONE), "disabling power save");

    EventBits_t bits = xEventGroupWaitBits(
        s_sta_events, STA_BIT_GOT_IP | STA_BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_OBSERVORE_WIFI_CONNECT_TIMEOUT_S * 1000));

    if (bits & STA_BIT_GOT_IP) {
        s_mode = OBSERVORE_MODE_UPLINK;
        s_mode_applied = true;
        return ESP_OK;
    }
    /* Stop the retry loop and tear the attempt down before returning.  s_mode
     * is still PATROL at this point (it is only advanced on success), so the
     * disconnect-on-leave in set_mode would not fire and the driver would keep
     * retrying the association into the next mode -- which showed up as two
     * patrol scans failing with ESP_ERR_WIFI_STATE and a stale failure
     * arriving nine seconds later. */
    s_connect_attempts = STA_MAX_RETRY;
    esp_wifi_disconnect();
    ESP_LOGW(TAG, "uplink did not come up within %ds: %s",
             CONFIG_OBSERVORE_WIFI_CONNECT_TIMEOUT_S,
             s_uplink_error[0] ? s_uplink_error : "no response from the network");
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* Mode handling                                                      */
/* ------------------------------------------------------------------ */

const char *observore_mode_name(observore_mode_t mode)
{
    switch (mode) {
        case OBSERVORE_MODE_CONSOLE: return "console";
        case OBSERVORE_MODE_UPLINK:  return "uplink";
        default:                 return "patrol";
    }
}

static void derive_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* The SSID carries the last three MAC bytes so two units in the same room
     * are distinguishable, and deliberately does not say "observore". */
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), CONFIG_OBSERVORE_AP_SSID_PREFIX "-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

uint32_t observore_wifi_sniffed_frames(void)
{
    return s_sniffed_frames;
}

uint32_t observore_wifi_sniffer_calls(void)
{
    return s_sniffer_calls;
}

/* Announce the device by name, so the console can be reached without first
 * hunting for an address that DHCP may have changed.
 *
 * mDNS is link-local multicast: it does not cross subnets.  A device on an
 * isolated IoT VLAN will not answer to clients on the main LAN unless the
 * router reflects mDNS between them. */
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(err));
        return;
    }

    snprintf(s_hostname, sizeof(s_hostname), "%s.local",
             CONFIG_OBSERVORE_HOSTNAME);
    /* Warned about rather than fatal, like the mdns_init() above and the
     * service registration below -- these two were the odd ones out. Losing
     * the .local name costs discoverability, not detection, and the DHCP
     * hostname still works. */
    err = mdns_hostname_set(CONFIG_OBSERVORE_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the mDNS hostname: %s", esp_err_to_name(err));
    }
    err = mdns_instance_name_set("Observore counter-surveillance");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the mDNS instance name: %s",
                 esp_err_to_name(err));
    }

    mdns_txt_item_t txt[] = {
        {"path", "/"},
    };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not advertise the console: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "answering to %s, and to \"%s\" via DHCP",
             s_hostname, CONFIG_OBSERVORE_HOSTNAME);
}

const char *observore_wifi_hostname(void)
{
    return s_hostname;
}

const char *observore_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *observore_wifi_ap_password(void)
{
    return observore_netcfg_ap_password();
}

esp_err_t observore_wifi_init(void)
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
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    /* Offer the name in the DHCP request too, not only over mDNS.  This is
     * what a router registers in its own DNS and lists as the client name --
     * and unlike mDNS multicast, ordinary DNS crosses subnets, which is the
     * case that actually matters when the device sits on an isolated VLAN.
     * Without it the router sees the ESP-IDF default, "espressif". */
    if (s_sta_netif) {
        esp_err_t herr = esp_netif_set_hostname(s_sta_netif,
                                                CONFIG_OBSERVORE_HOSTNAME);
        if (herr != ESP_OK) {
            ESP_LOGW(TAG, "could not set the DHCP hostname: %s",
                     esp_err_to_name(herr));
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    s_sta_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event_handler, NULL, NULL));
    /* Nothing here needs to survive a reboot, and writing the Wi-Fi config to
     * flash on every mode change would wear it for no reason. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    derive_ap_ssid();
    start_mdns();
    s_initialised = true;

    return observore_wifi_set_mode(OBSERVORE_MODE_PATROL);
}

size_t observore_wifi_last_scan(observore_scan_entry_t *out, size_t max)
{
    size_t n = s_last_scan_count < max ? s_last_scan_count : max;
    memcpy(out, s_last_scan, n * sizeof(*out));
    return n;
}

observore_mode_t observore_wifi_mode(void)
{
    return s_mode;
}

esp_err_t observore_wifi_set_mode(observore_mode_t mode)
{
    if (s_mode_applied && mode == s_mode) {
        return ESP_OK;
    }

    /* From here the radio is being reconfigured, so the current mode is no
     * longer applied.  Recording that before touching the driver is what lets
     * a caller fall back to the mode we are nominally already in and have it
     * actually take effect -- previously the guard above short-circuited that
     * call, leaving the station configured with the uplink SSID while we
     * believed we were patrolling. */
    s_mode_applied = false;

    /* Always leave promiscuous mode before touching the interface config --
     * changing mode underneath an active sniffer is how you get a driver
     * assert instead of an error code. */
    if (s_mode == OBSERVORE_MODE_UPLINK) {
        /* Disconnect explicitly before stopping, or the event handler retries
         * the association we are deliberately leaving. */
        s_connect_attempts = STA_MAX_RETRY;
        esp_wifi_disconnect();
    }
    s_uplink_ip[0] = '\0';
    s_uplink_connected = false;

    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();

    if (mode == OBSERVORE_MODE_UPLINK) {
        /* Apply exactly what was asked and report what happened.  Deciding
         * what to do instead is policy, and policy lives with the caller that
         * owns the LED, the web server and the retry backoff -- returning
         * ESP_OK after quietly doing something else forced that caller to
         * interrogate the driver to discover its own state. */
        return observore_wifi_uplink_connect();
    }

    if (mode == OBSERVORE_MODE_CONSOLE) {
        wifi_config_t ap = {0};
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        const char *ap_pw = observore_netcfg_ap_password();
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", ap_pw);
        ap.ap.channel = 6;
        ap.ap.max_connection = 2;
        /* The password is generated, so this is never the open branch in
         * practice; it stays as a guard against a short override. */
        ap.ap.authmode = strlen(ap_pw) >= 8 ? WIFI_AUTH_WPA2_PSK
                                            : WIFI_AUTH_OPEN;

        TRY(esp_wifi_set_mode(WIFI_MODE_AP), "softap mode");
        TRY(esp_wifi_set_config(WIFI_IF_AP, &ap), "softap config");
        TRY(esp_wifi_start(), "softap start");
        /* The SoftAP needs the receiver up continuously too, for the same
         * reason patrol does. */
        TRY(esp_wifi_set_ps(WIFI_PS_NONE), "disabling power save");
        ESP_LOGI(TAG, "console mode: SSID \"%s\" at 192.168.4.1", s_ap_ssid);
    } else {
        /* Patrol is station mode but deliberately never associated, which is
         * what leaves the radio free to be retuned to any channel.  The stored
         * SSID is cleared so WIFI_EVENT_STA_START does not try to join the
         * uplink we just left. */
        wifi_config_t blank = {0};
        TRY(esp_wifi_set_mode(WIFI_MODE_STA), "station mode");
        TRY(esp_wifi_set_config(WIFI_IF_STA, &blank), "clearing the station config");
        s_connect_attempts = STA_MAX_RETRY;
        TRY(esp_wifi_start(), "wifi start");
        /* Power save must be off to sniff.  The default WIFI_PS_MIN_MODEM
         * sleeps the receiver between beacons, which on an unassociated
         * station means it is asleep for nearly all of the sweep. */
        TRY(esp_wifi_set_ps(WIFI_PS_NONE), "disabling power save");
        ESP_LOGI(TAG, "patrol mode: scanning and sniffing");
    }

    s_mode = mode;
    s_mode_applied = true;
    return ESP_OK;
}

void observore_wifi_patrol_cycle(void (*between)(void))
{
    if (s_mode != OBSERVORE_MODE_PATROL) {
        return;
    }

#if SOC_WIFI_SUPPORT_5G
    /* Set once per cycle, and deliberately not once at init: the API returns
     * ESP_ERR_WIFI_NOT_STARTED on a stopped driver, and a mode change performs
     * an esp_wifi_stop()/start() that an init-time call would not survive --
     * the same trap the promiscuous callback below falls into.  Without this
     * the radio stays on 2.4 GHz and every 5 GHz channel is refused.
     *
     * It goes before run_ap_scan() rather than before the sniff sweep so that
     * the scan covers both bands as well.  A 5 GHz AP is found by the scan far
     * more cheaply than by waiting for a beacon to land in a sniff dwell. */
    esp_err_t berr = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (berr != ESP_OK) {
        ESP_LOGW(TAG, "could not enable dual-band scanning: %s",
                 esp_err_to_name(berr));
    }
#endif

    run_ap_scan();
    if (between) {
        between();   /* the scan alone blocks for about three seconds */
    }

    /* Sniff sweep.  Dwell is split evenly across the channels; a shorter dwell
     * covers the band faster but a beacon interval is typically ~102 ms, so
     * anything under about 120 ms per channel starts missing APs outright. */
    /* This cycle's sweep: the whole of 2.4 GHz, then the next slice of 5 GHz. */
    uint8_t sweep[ARRAY_COUNT(CHANNELS_2G) + OBSERVORE_SLICE_5G];
    size_t  n = 0;
    for (size_t i = 0; i < ARRAY_COUNT(CHANNELS_2G); i++) {
        sweep[n++] = CHANNELS_2G[i];
    }
#if SOC_WIFI_SUPPORT_5G
    for (size_t i = 0; i < OBSERVORE_SLICE_5G; i++) {
        sweep[n++]  = CHANNELS_5G[s_5g_cursor];
        s_5g_cursor = (s_5g_cursor + 1) % ARRAY_COUNT(CHANNELS_5G);
    }
#endif
    const int dwell_ms = OBSERVORE_SNIFF_MS / (int)n;

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
    esp_err_t serr = esp_wifi_set_promiscuous_filter(&filter);
    if (serr == ESP_OK) {
        serr = esp_wifi_set_promiscuous_rx_cb(sniffer_cb);
    }
    if (serr == ESP_OK) {
        serr = esp_wifi_set_promiscuous(true);
    }
    if (serr != ESP_OK) {
        /* Give up this sweep, not the device. BLE keeps running, the AP scan
         * above already happened, and the next cycle sets the sniffer up from
         * scratch anyway. */
        ESP_LOGW(TAG, "sniffer unavailable this cycle: %s", esp_err_to_name(serr));
        esp_wifi_set_promiscuous(false);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (s_mode != OBSERVORE_MODE_PATROL) {
            break;  /* a mode switch landed mid-sweep */
        }
        /* A channel the regulatory domain forbids is refused here rather than
         * being absent at compile time.  Skip it instead of dwelling on a
         * channel the radio never actually moved to. */
        esp_err_t cerr = esp_wifi_set_channel(sweep[i], WIFI_SECOND_CHAN_NONE);
        if (cerr != ESP_OK) {
            ESP_LOGD(TAG, "channel %u unavailable: %s",
                     sweep[i], esp_err_to_name(cerr));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));
        if (between) {
            between();   /* BLE keeps finding things during the sweep */
        }
    }
    esp_wifi_set_promiscuous(false);
}
