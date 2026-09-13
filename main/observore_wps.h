#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What an access point says about itself in its own beacon.
 *
 * A MAC prefix is an inference: this address block belongs to that company, so
 * the device is probably theirs. The WPS information element is not an
 * inference. The device states its manufacturer, model and name in plain text,
 * and it is present in every beacon from an access point that supports Wi-Fi
 * Protected Setup, which is most consumer hardware and a great many cameras.
 *
 * That matters most exactly where the OUI is weakest: a camera whose vendor
 * prefix is unassigned or unknown still announces "Manufacturer: Hikvision"
 * to anyone listening.
 *
 * Every field here comes from an untrusted radio frame, so it is length
 * checked, bounds checked and stripped of anything unprintable before it
 * reaches a log line, a JSON response or an HTML page.
 */
#define OBSERVORE_WPS_FIELD_LEN 33   /* 32 usable + NUL */

typedef struct {
    char manufacturer[OBSERVORE_WPS_FIELD_LEN];
    char model[OBSERVORE_WPS_FIELD_LEN];
    char device_name[OBSERVORE_WPS_FIELD_LEN];
} observore_wps_t;

/* Finds the WPS element among a frame's tagged information elements and pulls
 * out what it says. Returns true when at least one field was populated.
 * `out` is always fully initialised, whatever the return value. */
bool observore_wps_from_ies(const uint8_t *ies, size_t len, observore_wps_t *out);

/* True when nothing was found. */
bool observore_wps_empty(const observore_wps_t *w);
