#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Starts the NimBLE host and an indefinite passive scan.  Passive is the whole
 * point: the controller never sends SCAN_REQ, so nothing being watched can see
 * that it is being watched. */
esp_err_t observore_ble_start(void);

/* Stop and restart the passive scan.
 *
 * Used while an update is downloading. The sniffer is already suspended on the
 * uplink, so the scan is the only thing still competing for the one radio, and
 * a firmware download that shares the antenna with a BLE scan is slower and
 * more likely to stall in exactly the place a stall is expensive.
 *
 * The host is left running rather than torn down: a successful update reboots,
 * and a failed one has to put the detector back exactly as it was. */
esp_err_t observore_ble_pause(void);
esp_err_t observore_ble_resume(void);
