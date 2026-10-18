#pragma once

#include "observore_types.h"

/* The detection log, on a card, as CSV.
 *
 * NVS holds 48 history entries. That is a property of where it is kept
 * rather than of anything the detector knows -- a board watching a place
 * for a week has far more to say, and throws almost all of it away. A
 * sixteen-gigabyte card does not have that problem.
 *
 * CSV because the point of taking the card out is to read it somewhere
 * else, and every machine already opens CSV. A binary log would be smaller
 * and would need a tool that only this project ships.
 *
 * ## What is written down about the risk
 *
 * This file is a list of who was near the device and when. That is a
 * tracking record of everyone who passed, in a form anybody can read on any
 * laptop, held on a thing that can be pocketed -- a different exposure from
 * the same data in NVS, which needs a cable and a flash reader.
 *
 * It is logged anyway, deliberately: every byte of it was broadcast
 * unencrypted by the devices themselves, to anybody in range who cared to
 * listen. The card does not create the exposure, it records one that was
 * already public. #185 covers secrets at rest, which is a different
 * question -- a Wi-Fi password is a secret, a beacon is a shout.
 *
 * Off unless a board profile turns it on.
 */

void observore_sdlog_init(void);

/* Append one detection. Safe to call when there is no card, no slot, or
 * logging is off: it does nothing. */
void observore_sdlog_note(const observore_event_t *e);

/* How many records have been written since boot, and how many were lost
 * because the card would not take them. */
void observore_sdlog_counts(uint32_t *written, uint32_t *dropped);
