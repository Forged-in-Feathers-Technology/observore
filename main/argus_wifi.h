#pragma once

#include "esp_err.h"

/* The ESP32-S3 has one radio on one channel.  Channel-hopping to sniff and
 * staying associated to an AP are therefore mutually exclusive, which is why
 * Argus has modes rather than a single loop.
 *
 * BLE scanning runs continuously in every mode -- it is unaffected by the
 * Wi-Fi channel, and it is where most detections come from. */
typedef enum {
    ARGUS_MODE_PATROL = 0,  /* unassociated: AP scans + channel-hopping sniff */
    ARGUS_MODE_CONSOLE,     /* SoftAP + web UI; sniffing suspended */
} argus_mode_t;

esp_err_t argus_wifi_init(void);

/* Switch modes.  Safe to call with the mode already active (no-op). */
esp_err_t argus_wifi_set_mode(argus_mode_t mode);
argus_mode_t argus_wifi_mode(void);

/* Run one patrol cycle: an active AP scan followed by a promiscuous sniff
 * sweep across the 2.4 GHz channels.  Blocks for roughly
 * ARGUS_SCAN_MS + ARGUS_SNIFF_MS.  No-op outside patrol mode. */
void argus_wifi_patrol_cycle(void);

/* Management frames accepted by the sniffer since boot.  A patrol cycle that
 * leaves this unchanged means the sniffer is not hearing air, which looks
 * exactly like a quiet neighbourhood unless you can see the number. */
uint32_t argus_wifi_sniffed_frames(void);

/* Raw callback entries, before any parsing.  Compared against the accepted
 * count this says whether a silent sniffer is not receiving or is being
 * rejected by our own frame handling. */
uint32_t argus_wifi_sniffer_calls(void);

const char *argus_wifi_ap_ssid(void);
const char *argus_wifi_ap_password(void);
