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

/* Whether the device knows what time it is.  Everything that reports a time
 * must check this: emitting 1970 dressed up as a timestamp would be worse
 * than admitting the clock is unset, on a device whose output is evidence. */
bool observore_clock_valid(void);

/* Wall-clock UTC for a monotonic timestamp, or 0 when the time is unknown. */
time_t observore_clock_at(int64_t uptime_us);

/* ISO-8601 UTC ("2026-09-12T18:04:11Z") for a monotonic timestamp.  False,
 * with out set to an empty string, when the time is unknown. */
bool observore_clock_iso(int64_t uptime_us, char *out, size_t len);

/* When the clock was last set, as a monotonic timestamp; 0 if never. */
int64_t observore_clock_synced_at(void);
