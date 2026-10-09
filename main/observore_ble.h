#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "observore_peer.h"

/* Starts the NimBLE host and an indefinite passive scan.  Passive is the whole
 * point: the controller never sends SCAN_REQ, so nothing being watched can see
 * that it is being watched. */
esp_err_t observore_ble_start(void);

/* Stop the BLE stack entirely, and start it again with observore_ble_start().
 *
 * Used while an update is downloading. Cancelling the scan is not enough: the
 * controller and host keep their buffers either way, and those buffers are
 * DMA-capable internal memory, which is exactly what the TLS session needs and
 * cannot take from PSRAM. With the stack resident the hardware AES driver fails
 * its allocation and the download never starts.
 *
 * Nothing is lost by stopping. The sniffer is already suspended on the uplink,
 * so a device downloading firmware is not detecting anything regardless.
 *
 * One way, for this boot. A finished update reboots and a failed one reboots
 * too, so there is no resume path to get wrong on the one code path that only
 * runs when something has already gone wrong. */
esp_err_t observore_ble_stop(void);

/* Whether this build can transmit at all.
 *
 * False unless CONFIG_OBSERVORE_MESH_TX is set, and false in a way that is
 * not a flag: without that option the NimBLE broadcaster role is not
 * compiled, so there is no code here that could advertise. Everything that
 * reports the device's state asks this rather than reading the config, so
 * what is shown and what is possible cannot drift apart. */
bool observore_ble_can_warn(void);

/* Send one warning burst, if this build can.
 *
 * The sequence number is assigned here, so a caller cannot replay its own
 * warning by passing the same one twice -- the receiving side drops a
 * sequence that does not advance, and that check is worth nothing if the
 * sender keeps repeating a number.
 *
 * Returns false when the build cannot transmit, the host is not ready, or the
 * radio refused. */
bool observore_ble_warn(const observore_peer_warning_t *w);
