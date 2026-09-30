#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether the place around the device has changed.
 *
 * Access points are the reference frame this device already has: stationary
 * by definition, plentiful indoors, and scanned every fifteen seconds while
 * patrolling. Which ones are in earshot answers a question no accelerometer
 * can -- not "did I move" but "did I go anywhere" -- and the difference
 * between those two is what the tailing class got wrong the first time.
 *
 * Carrying the board around a house leaves the same access points in range,
 * so every follower in the building looked like it had come along. Taking it
 * somewhere else replaces nearly all of them.
 *
 * No ESP-IDF here on purpose: this is set arithmetic, and the host tests
 * drive it directly. */

/* Forget everything. */
void observore_surroundings_reset(void);

/* An access point was just seen, and how loudly. `hash` identifies it -- a
 * BSSID reduced to 32 bits, since nothing here needs to name one, only to
 * recognise it. */
void observore_surroundings_note(uint32_t hash, int rssi, int64_t now_us);

/* Remember what is in earshot now, as the set to compare against later.
 * Access points heard longer ago than `max_age_us` are left out: a stale
 * entry from the place you left would make the new place look familiar. */
void observore_surroundings_mark(int64_t now_us, int64_t max_age_us);

/* How much of the marked set has been heard again since it was marked, as a
 * percentage. -1 when nothing was marked -- an honest "cannot say", which a
 * caller must not read as either answer. */
int observore_surroundings_overlap_pct(void);

/* How many access points were in the marked set. */
size_t observore_surroundings_marked(void);

/* How much fainter the access points from before have become, in decibels,
 * across those heard at both ends. Positive means quieter than before.
 *
 * Membership alone cannot answer "did I go anywhere" in the countryside: the
 * only access points for half a mile may be the ones in your own house, and
 * they still reach the barn. Eighty percent of them were still audible from
 * a barn down the driveway, so the journey did not count -- correct by the
 * rule and wrong about the world. The same access points, all of them much
 * fainter, is distance.
 *
 * Returns INT_MIN when too few were heard at both ends to say. The median
 * rather than the mean, because one access point going quiet behind a
 * tractor should not carry the answer. */
int observore_surroundings_faded_db(void);
