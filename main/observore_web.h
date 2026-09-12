#pragma once

#include "esp_err.h"

/* Starts the console HTTP server on the SoftAP at 192.168.4.1.  Safe to call
 * when already running. */
esp_err_t observore_web_start(void);
esp_err_t observore_web_stop(void);
