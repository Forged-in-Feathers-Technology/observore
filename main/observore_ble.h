#pragma once

#include <stdbool.h>

#include "esp_err.h"

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
