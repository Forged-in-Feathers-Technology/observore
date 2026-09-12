#pragma once

#include <stddef.h>
#include <stdint.h>

#include "observore_types.h"

/* Helpers shared by the ESP-only modules.  Anything the host test build needs
 * lives in observore_detect.h instead, so test/Makefile pulls in no ESP-IDF. */

/* Escape a string for embedding in a JSON document.  Everything this is
 * applied to is remote-controlled -- advertised names, SSIDs, transport error
 * text -- so it is not optional.  Non-printable bytes become spaces rather
 * than broken escapes. */
void observore_json_escape(const char *in, char *out, size_t out_len);

#define OBSERVORE_MAC_STR_LEN 18   /* "AA:BB:CC:DD:EE:FF" + NUL */

/* Format a MAC into out, which must be at least OBSERVORE_MAC_STR_LEN, and
 * return it -- so a call site spends one argument rather than six indices. */
const char *observore_mac_str(const uint8_t mac[OBSERVORE_MAC_LEN],
                              char out[OBSERVORE_MAC_STR_LEN]);
