#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Starts the console HTTP server on the SoftAP at 192.168.4.1.  Safe to call
 * when already running. */
esp_err_t observore_web_start(void);
esp_err_t observore_web_stop(void);

/* When the console was last fetched from, in esp_timer microseconds, or 0.
 * The uplink window is held open while someone is actually reading it. */
int64_t observore_web_last_request_us(void);
