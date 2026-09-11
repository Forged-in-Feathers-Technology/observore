#pragma once

#include "esp_err.h"

/* Starts the NimBLE host and an indefinite passive scan.  Passive is the whole
 * point: the controller never sends SCAN_REQ, so nothing being watched can see
 * that it is being watched. */
esp_err_t argus_ble_start(void);
