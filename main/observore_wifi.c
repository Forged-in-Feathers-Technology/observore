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

static const char *TAG = "observore.wifi";

#define OBSERVORE_SCAN_MS      3000
#define OBSERVORE_SNIFF_MS     5000
#define OBSERVORE_CHANNEL_MIN  1
#define OBSERVORE_CHANNEL_MAX  13
#define OBSERVORE_MAX_AP       32

/* ASTM F3411 Remote ID over Wi-Fi: a vendor-specific IE (element 0xDD) whose
 * OUI is FA:0B:BC with vendor type 0x0D. */
static const uint8_t ASTM_OUI[3]     = {0xFA, 0x0B, 0xBC};
#define ASTM_VENDOR_TYPE_ODID 0x0D
#define IE_VENDOR_SPECIFIC    0xDD
#define IE_SSID               0x00

static observore_mode_t       s_mode = OBSERVORE_MODE_PATROL;
static esp_netif_t       *s_ap_netif;
static char               s_ap_ssid[32];
static char               s_hostname[48];
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
        .addr_random = observore_mac_is_random(hdr->addr2),
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
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* scan_time is deliberately left at zero.  With Bluetooth enabled the
         * driver rejects custom active-scan timing outright ("Should use
         * default active scan time parameter") and the scan returns nothing,
         * so the coexistence arbiter picks the dwell instead. */
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

    int64_t now = esp_timer_get_time();
    for (uint16_t i = 0; i < count; i++) {
        observore_observation_t obs = {
            .mac     = records[i].bssid,
            .addr_random = observore_mac_is_random(records[i].bssid),
            .src     = OBSERVORE_SRC_WIFI_SCAN,
            .rssi    = records[i].rssi,
            .channel = records[i].primary,
            .ssid    = (const char *)records[i].ssid,
        };
        observore_track_observe(&obs, now);
    }
    ESP_LOGI(TAG, "scan: %u APs", count);
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    memset(&sta, 0, sizeof(sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

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
             CONFIG_OBSERVORE_MDNS_HOSTNAME);
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_OBSERVORE_MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Observore counter-surveillance"));

    mdns_txt_item_t txt[] = {
        {"path", "/"},
    };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not advertise the console: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "answering to %s", s_hostname);
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
    return CONFIG_OBSERVORE_AP_PASSWORD;
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
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

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

observore_mode_t observore_wifi_mode(void)
{
    return s_mode;
}

esp_err_t observore_wifi_set_mode(observore_mode_t mode)
{
    if (s_mode_applied && mode == s_mode) {
        return ESP_OK;
    }

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
        esp_err_t err = observore_wifi_uplink_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uplink unavailable, staying on patrol");
            return observore_wifi_set_mode(OBSERVORE_MODE_PATROL);
        }
        return ESP_OK;
    }

    if (mode == OBSERVORE_MODE_CONSOLE) {
        wifi_config_t ap = {0};
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s",
                 CONFIG_OBSERVORE_AP_PASSWORD);
        ap.ap.channel = 6;
        ap.ap.max_connection = 2;
        ap.ap.authmode = strlen(CONFIG_OBSERVORE_AP_PASSWORD) >= 8
                             ? WIFI_AUTH_WPA2_PSK
                             : WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        /* The SoftAP needs the receiver up continuously too, for the same
         * reason patrol does. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_LOGI(TAG, "console mode: SSID \"%s\" at 192.168.4.1", s_ap_ssid);
    } else {
        /* Patrol is station mode but deliberately never associated, which is
         * what leaves the radio free to be retuned to any channel.  The stored
         * SSID is cleared so WIFI_EVENT_STA_START does not try to join the
         * uplink we just left. */
        wifi_config_t blank = {0};
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &blank));
        s_connect_attempts = STA_MAX_RETRY;
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

void observore_wifi_patrol_cycle(void)
{
    if (s_mode != OBSERVORE_MODE_PATROL) {
        return;
    }

    run_ap_scan();

    /* Sniff sweep.  Dwell is split evenly across the channels; a shorter dwell
     * covers the band faster but a beacon interval is typically ~102 ms, so
     * anything under about 120 ms per channel starts missing APs outright. */
    const int channels = OBSERVORE_CHANNEL_MAX - OBSERVORE_CHANNEL_MIN + 1;
    const int dwell_ms = OBSERVORE_SNIFF_MS / channels;

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
    for (int ch = OBSERVORE_CHANNEL_MIN; ch <= OBSERVORE_CHANNEL_MAX; ch++) {
        if (s_mode != OBSERVORE_MODE_PATROL) {
            break;  /* a mode switch landed mid-sweep */
        }
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    }
    esp_wifi_set_promiscuous(false);
}
