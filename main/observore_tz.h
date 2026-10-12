#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Which timezone the device reads its own clock in.
 *
 * It was a build-time string, CONFIG_OBSERVORE_TZ, applied once with setenv
 * and tzset. That is the wrong place for it: the people this is for flash a
 * release from a browser, so a timezone compiled into the firmware is a
 * timezone nobody can correct -- and unlike most preferences this one is not
 * cosmetic. The census counts days in local time, so a device in the wrong
 * zone draws its day boundary in the wrong place and the sixteen-day
 * membership window is measured against the wrong dates.
 *
 * ## Changing it does not rewrite history, and that was already true
 *
 * A timezone change can move the day index backwards, and the census already
 * refuses to rewrite a window it cannot reconstruct -- see age_to() in
 * observore_census.c, which names a timezone change as one of the two reasons
 * the clock can go backwards. So a change here applies to the days that follow
 * it and leaves the days already counted as they were. Nothing in this module
 * had to arrange that; it is worth writing down because it is the property
 * that makes a runtime timezone safe at all.
 *
 * ## POSIX counts the other way, and that is the trap
 *
 * In a POSIX TZ string the offset is how far the zone is *behind* UTC, so a
 * zone five hours east of UTC is written "UTC-5". Everybody gets this wrong
 * once. The host tests do not take anybody's word for it: they build a string,
 * hand it to tzset, and check what localtime_r then makes of a known instant.
 *
 * ## Why a POSIX string and not a zone name
 *
 * Because ESP-IDF ships no zone database. "America/Toronto" means nothing to
 * newlib. A full POSIX string can carry daylight-saving rules --
 * "EST5EDT,M3.2.0,M11.1.0" -- and anybody who knows their zone's rules can set
 * exactly that through the console. What a screen can offer is a whole-hour
 * offset, which is correct all year in zones that do not shift and wrong by an
 * hour for half the year in zones that do. That limit is stated where it is
 * offered rather than hidden.
 */

/* "EST5EDT,M3.2.0,M11.1.0" is 22 characters; this leaves room for the longer
 * rules and the angle-bracket form. */
#define OBSERVORE_TZ_MAX 48

/* Whether a string is one this device will accept as a POSIX TZ.
 *
 * Deliberately narrow. The value reaches setenv, the console, the serial log
 * and a 40-column panel, so it is restricted to the characters POSIX actually
 * uses and nothing that could be mistaken for anything else. */
bool observore_tz_valid(const char *tz);

/* A fixed-offset POSIX string for a zone `east_minutes` east of UTC. Negative
 * is west. The sign of the string it writes is the opposite of the sign of the
 * argument, which is POSIX's doing and not a mistake. */
bool observore_tz_from_offset(int east_minutes, char *out, size_t out_len);

/* The offset a fixed-offset string encodes, in minutes east of UTC, or
 * OBSERVORE_TZ_NOT_FIXED when the string is something richer than that -- a
 * zone with daylight-saving rules has no one offset to report. */
#define OBSERVORE_TZ_NOT_FIXED (-100000)
int observore_tz_offset_of(const char *tz);

/* The string in use. Never NULL. */
const char *observore_tz_get(void);

/* Store and apply, refusing anything observore_tz_valid rejects. */
bool observore_tz_set(const char *tz);

/* Load the stored zone, or the build's default when nothing is stored, and
 * apply it. Call once, before anything reads a local time. */
void observore_tz_init(void);
