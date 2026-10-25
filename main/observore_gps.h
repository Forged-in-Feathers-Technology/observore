#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* A GPS module, read for the time rather than for the position.
 *
 * ## Why a detector that sits still wants a GPS
 *
 * Not for where it is. For what time it is, which is the thing eight of the
 * nine boards cannot find out on their own.
 *
 * Only the round AMOLED has a clock chip. Every other board cold boots into
 * 1970 and stays there until something tells it otherwise, and the two things
 * that can are a browser at the console and SNTP. A device on a camera VLAN
 * with no route out never hears from an NTP server -- the same isolation that
 * stops a notification reaching its provider -- so it sits in the place it is
 * meant to watch, with no idea what day it is.
 *
 * That is not a cosmetic gap. The census counts a device as household on three
 * distinct days inside a sixteen-day window, and it cannot count a day without
 * a trusted clock. No clock means the household table never fills, which means
 * nothing is ever recognised as furniture, which means the thing this device
 * exists to do -- report what is new -- never starts working.
 *
 * A GPS fixes that with no network at all, permanently, and it keeps working on
 * a VLAN with no route anywhere. That is the argument. Position is a bonus that
 * #132 can use later.
 *
 * ## What this half does, and what it does not
 *
 * Reads sentences and reports what it sees. It does not set the clock yet.
 *
 * That split is the one the census and the mesh both shipped in, and here it is
 * forced by honesty as much as by caution: a module indoors sees no satellites
 * at all, so the path from "a fix arrived" to "the clock is set" cannot be
 * watched from a desk. What can be watched is everything up to it, and that is
 * what this is.
 *
 * ## The pin was measured, not looked up
 *
 * GPIO 4 at 9600 baud on the NM-CYD-C5, found by configuring UART RX on every
 * free pin in turn and listening for a sentence. The same probe reported GPIOs
 * 15 to 22 as reserved by the system, which is flash and PSRAM: asking
 * esp_gpio_is_reserved() rather than assuming is what kept the board up.
 *
 * This matters because ~/.observore-boards carries a warning about exactly this
 * -- a pin map written from an assumption was confirmed as correct every time,
 * forever, and cost a day.
 *
 * ## What the bench module emits
 *
 * GGA, GLL, RMC, VTG, ZDA, GSA, GSV and a TXT antenna status, all with the GN
 * talker. RMC is the one that matters: it carries the date, the time and a
 * validity flag in one sentence. GGA has a time field but no date, so GGA alone
 * could never set a clock.
 */

/* NMEA allows 82 characters including the leading $ and the trailing CRLF. */
#define OBSERVORE_NMEA_MAX 84

/* Assembles bytes into sentences. */
typedef struct {
    char    buf[OBSERVORE_NMEA_MAX];
    uint8_t len;
    uint32_t too_long;    /* sentences discarded for exceeding the limit */
} observore_nmea_scan_t;

/* What the module has told us. */
typedef struct {
    bool    time_valid;        /* RMC reported A, and the date and time parsed */
    int     year, mon, day;    /* year is four digits */
    int     hour, min, sec;    /* UTC, which is what NMEA carries */
    uint8_t fix_quality;       /* GGA field 6: 0 is no fix */
    uint8_t satellites;        /* GGA field 7 */
    uint8_t in_view;           /* GSV field 3 */
    bool    antenna_ok;        /* a TXT saying so; false until one arrives */
    bool    antenna_seen;
    uint32_t sentences;        /* checksum-valid sentences understood */
    uint32_t bad_checksum;
    /* Where the module says it is, in degrees times ten million -- the same
     * unit the mesh format carries, so nothing is converted between them.
     * Only set while there is a fix; cleared by any GGA without one, so a
     * position can never outlive the fix that produced it. */
    bool    have_pos;
    int32_t lat_e7, lon_e7;
} observore_gps_t;

/* Feed one byte. Returns a complete sentence when one finishes, or NULL.
 *
 * The returned pointer is into the scanner and is valid until the next call.
 * The checksum is NOT verified here -- that is a separate question with a
 * separate answer, and a caller that forgets to ask is a caller that sets a
 * clock from line noise. */
const char *observore_nmea_feed(observore_nmea_scan_t *s, char c);

/* Whether a sentence's trailing *XX checksum matches its body.
 *
 * Never optional. A GPS shares a cheap serial line with whatever else is on
 * the bench, and a single flipped bit in a date field is a clock set to the
 * wrong century. */
bool observore_nmea_checksum_ok(const char *sentence);

/* Apply a sentence to the state. Returns false for a sentence this build does
 * not read, which is not an error -- a module emits plenty that is of no use
 * here. Verify the checksum first. */
bool observore_gps_apply(observore_gps_t *g, const char *sentence);

/* The UTC the module last reported, as a struct tm, or false when it has not
 * reported a valid one. Fills tm_year and tm_mon the way the C library wants
 * them, which is not the way NMEA gives them. */
bool observore_gps_utc(const observore_gps_t *g, struct tm *out);

#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#if CONFIG_OBSERVORE_GPS
/* Start reading the module. Costs a UART, a task and a kilobyte of ring, so it
 * exists only on a board built with a module wired to it. */
void observore_gps_init(void);
/* What it has seen so far, for the console and the screen. Safe to call from
 * any task. */
observore_gps_t observore_gps_state(void);
#endif
#endif
