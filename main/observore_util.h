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

/* An append cursor for building a JSON document.
 *
 * Every handler used to carry its own `n += snprintf(body + n, CAP - n, ...)`
 * arithmetic and its own truncation policy, and they disagreed: one logged and
 * stopped, one stopped silently, one never checked at all.  A `sizeof` on what
 * had become a pointer -- in exactly that expression -- once shipped
 * /api/devices without its closing bracket, which the browser refused to parse
 * and which looked for all the world like a network fault.
 *
 * The cursor removes the arithmetic from call sites and, more usefully, holds
 * back `reserve` bytes at construction so the closing sequence is guaranteed to
 * fit.  Running out of room therefore yields a short but *parseable* document
 * rather than a broken one.  Writes after the buffer fills are silently
 * dropped, so a caller may keep going and close normally. */
typedef struct {
    char  *buf;
    size_t cap;      /* usable capacity, already less the reserve */
    size_t len;
    bool   full;
} observore_jbuf_t;

void observore_jb_init(observore_jbuf_t *jb, char *buf, size_t size,
                       size_t reserve);

/* Append formatted text.  No-op once full. */
void observore_jb_printf(observore_jbuf_t *jb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Append a string with JSON escaping applied, without surrounding quotes. */
void observore_jb_escape(observore_jbuf_t *jb, const char *s);

static inline bool observore_jb_full(const observore_jbuf_t *jb)
{
    return jb->full;
}

/* Write the closing sequence using the reserved bytes.  Always fits. */
void observore_jb_close(observore_jbuf_t *jb, const char *tail);

#define OBSERVORE_MAC_STR_LEN 18   /* "AA:BB:CC:DD:EE:FF" + NUL */

/* Format a MAC into out, which must be at least OBSERVORE_MAC_STR_LEN, and
 * return it -- so a call site spends one argument rather than six indices. */
const char *observore_mac_str(const uint8_t mac[OBSERVORE_MAC_LEN],
                              char out[OBSERVORE_MAC_STR_LEN]);
