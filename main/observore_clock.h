#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Absolute time, for a device whose whole output is "what, and when".
 *
 * Everything internal is timestamped with esp_timer_get_time() -- microseconds
 * since boot, monotonic, and available immediately.  This module converts one
 * of those into wall-clock time once the device has learned what time it is.
 *
 * The conversion is deliberately retroactive.  Because it works from the
 * monotonic delta rather than from a timestamp captured at sync, a sighting
 * recorded before the clock was set can still be dated correctly afterwards --
 * which matters, because the device starts detecting the moment it boots and
 * only learns the time when it next reaches the network.
 */
void observore_clock_init(void);

/* Where the time came from.
 *
 * This exists because "the system clock holds a plausible number" and "the
 * device knows what time it is" are different claims, and the first one used
 * to stand in for the second. A seeded or guessed time would have satisfied
 * it, and the consequences are worse than a wrong timestamp: the census
 * counts separate days, so a clock that never advances records every sighting
 * on one day, and a clock set to the wrong day records furniture against a
 * date that is not the one it will be judged against.
 *
 * So the clock says what set it, and only a real source counts. The order is
 * worst to best and is used as a ranking: a better source may overwrite a
 * worse one without being asked, which is how a person's rough answer gets
 * quietly corrected when the network finally arrives. */
typedef enum {
    OBSERVORE_CLOCK_NONE = 0,   /* never set: the time is whatever boot left  */
    OBSERVORE_CLOCK_PERSON,     /* handed in from a browser at the console    */
    OBSERVORE_CLOCK_CHIP,       /* read from the board's own RTC at startup   */
    OBSERVORE_CLOCK_NETWORK,    /* SNTP, which is the one that is actually right */
} observore_clock_source_t;

observore_clock_source_t observore_clock_source(void);
const char *observore_clock_source_name(observore_clock_source_t src);

/* Whether the device knows what time it is.  Everything that reports a time
 * must check this: emitting 1970 dressed up as a timestamp would be worse
 * than admitting the clock is unset, on a device whose output is evidence.
 *
 * True only when something actually set the clock AND the result is a
 * plausible date. Both halves are needed: a source with no clock behind it is
 * what a power cycle leaves, and a plausible clock with no source is what a
 * seed would leave. */
bool observore_clock_valid(void);

typedef enum {
    OBSERVORE_CLOCK_TAKE = 0,       /* use it                                */
    OBSERVORE_CLOCK_OUT_OF_RANGE,   /* no running device is at that instant   */
    OBSERVORE_CLOCK_WORSE,          /* something better already set the clock */
} observore_clock_ruling_t;

/* Set the clock by hand, from a person at the console.
 *
 * The one way a board with no RTC chip and no reachable NTP server can be
 * dated at all -- which is most of them, in most of the places this device is
 * worth carrying. The browser asking already knows the time to the
 * millisecond, and a person's phone is a better clock than no clock.
 *
 * It is a *source*, not an override: a time that the device could not
 * plausibly be running at is refused rather than stored, and a later SNTP
 * reply replaces it without asking.
 *
 * Returns the ruling rather than a bool, because the two ways of being
 * refused are different things to tell a person: a time outside the window is
 * theirs to correct, and a clock already set from somewhere better is not a
 * problem at all. One message covering both was what the console showed at
 * first, and it could not say which had happened. TAKE is zero and is the
 * only success. */
observore_clock_ruling_t observore_clock_set(time_t when,
                                             observore_clock_source_t src);

/* The decision inside observore_clock_set(), separated from the act of
 * carrying it out.
 *
 * Pure, and therefore testable: the rest of this module needs a real
 * settimeofday and a chip on an I2C bus, which is how a rule like this ends
 * up never exercised anywhere. `have` and `have_time` describe the clock as
 * it stands -- its source, and whether what it currently reads is inside the
 * window at all. */
observore_clock_ruling_t observore_clock_rule(observore_clock_source_t have,
                                              bool have_time,
                                              observore_clock_source_t src,
                                              time_t when);

/* The window a running device's clock can plausibly be in, as instants.
 *
 * Exposed so that the tests name the same boundaries the code enforces rather
 * than restating constants that could drift apart from it. */
#define OBSERVORE_CLOCK_SANE_FROM  1735689600L   /* 2025-01-01T00:00:00Z */
#define OBSERVORE_CLOCK_SANE_UNTIL 4102444800L   /* 2100-01-01T00:00:00Z */

/* Wall-clock UTC for a monotonic timestamp, or 0 when the time is unknown. */
time_t observore_clock_at(int64_t uptime_us);

/* ISO-8601 UTC ("2026-09-12T18:04:11Z") for a monotonic timestamp.  False,
 * with out set to an empty string, when the time is unknown. */
bool observore_clock_iso(int64_t uptime_us, char *out, size_t len);

/* When the clock was last set, as a monotonic timestamp; 0 if never. */
int64_t observore_clock_synced_at(void);

/* The local day the census may count against, or OBSERVORE_CENSUS_NO_DAY.
 *
 * The one door between the clock and the census, and it is here rather than
 * in the census so that there is only one. `observore_census_day()` converts
 * whatever timestamp it is handed and is right to: it is pure arithmetic and
 * the host tests drive it directly. Deciding whether the timestamp is worth
 * believing is a question about this module, and asking it at four separate
 * call sites is how three of them end up still asking the old way.
 *
 * Returns NO_DAY whenever the clock has no source, which is what makes a
 * guessed or seeded time unable to teach the census anything -- a clock that
 * never advances would otherwise record every sighting on one day forever. */
int observore_clock_day(void);
