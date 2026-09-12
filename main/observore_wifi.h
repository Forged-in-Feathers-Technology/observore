#pragma once

#include "esp_err.h"

/* The ESP32-S3 has one radio on one channel.  Channel-hopping to sniff and
 * staying associated to an AP are therefore mutually exclusive, which is why
 * Observore has modes rather than a single loop.
 *
 * BLE scanning runs continuously in every mode -- it is unaffected by the
 * Wi-Fi channel, and it is where most detections come from. */
typedef enum {
    OBSERVORE_MODE_PATROL = 0,  /* unassociated: AP scans + channel-hopping sniff */
    OBSERVORE_MODE_CONSOLE,     /* SoftAP + web UI; sniffing suspended */
    OBSERVORE_MODE_UPLINK,      /* joined to your network; sniffing suspended */
} observore_mode_t;

const char *observore_mode_name(observore_mode_t mode);

/* Join the configured network.  Blocks up to the configured timeout and
 * returns ESP_ERR_NOT_FOUND when no credentials are set, ESP_ERR_TIMEOUT when
 * association or DHCP did not complete.  On failure the caller is expected to
 * fall back to patrol rather than sit associated to nothing. */
esp_err_t observore_wifi_uplink_connect(void);

/* The address acquired in uplink mode, or an empty string. */
const char *observore_wifi_uplink_ip(void);

/* Why the last uplink attempt failed, in plain words, or an empty string. */
const char *observore_wifi_uplink_error(void);

/* True while the station actually holds an address.  Distinct from being in
 * uplink mode: the mode can outlive the association when the access point goes
 * away, and sitting in a mode that cannot transmit while the sniffer is
 * switched off is the worst of both. */
bool observore_wifi_uplink_connected(void);

esp_err_t observore_wifi_init(void);

/* Switch modes.  Safe to call with the mode already active (no-op). */
esp_err_t observore_wifi_set_mode(observore_mode_t mode);
observore_mode_t observore_wifi_mode(void);

/* Run one patrol cycle: an active AP scan followed by a promiscuous sniff
 * sweep across the 2.4 GHz channels.  Blocks for roughly
 * OBSERVORE_SCAN_MS + OBSERVORE_SNIFF_MS.  No-op outside patrol mode. */
void observore_wifi_patrol_cycle(void);

/* Management frames accepted by the sniffer since boot.  A patrol cycle that
 * leaves this unchanged means the sniffer is not hearing air, which looks
 * exactly like a quiet neighbourhood unless you can see the number. */
uint32_t observore_wifi_sniffed_frames(void);

/* Raw callback entries, before any parsing.  Compared against the accepted
 * count this says whether a silent sniffer is not receiving or is being
 * rejected by our own frame handling. */
uint32_t observore_wifi_sniffer_calls(void);

const char *observore_wifi_ap_ssid(void);
const char *observore_wifi_ap_password(void);
