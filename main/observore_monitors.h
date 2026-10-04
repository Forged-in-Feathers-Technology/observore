#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "observore_types.h"

/* Which questions the device is asking.
 *
 * Not everybody wants every class. Somebody watching a car park for trackers
 * has no use for fleet telematics; somebody auditing a building's cameras
 * does not want a follower class at all. So each monitor can be switched off
 * (#146).
 *
 * ## Off means "keep seeing, stop reporting"
 *
 * A disabled class is still classified and still counted. What it does not do
 * is contribute to the score, appear in the findings, or go out in a
 * notification. That costs almost nothing -- the device table is shared, so
 * there is no extra storage -- and it means switching a monitor back on shows
 * immediately what has been around all along, rather than starting a fresh
 * and silent history. It is the same shape as the census: gather first, act
 * later.
 *
 * ## The rule this must not break
 *
 * **A device with monitors off must never report "clear" as though it had
 * looked.** This project has twice silenced the thing it exists to notice,
 * and both times the suppression did not announce itself. This is worse than
 * either, because a disabled monitor leaves nothing in the data to find
 * later: a mute rule at least carries a count of what it suppressed. So the
 * count of disabled monitors travels with the verdict everywhere the verdict
 * goes.
 *
 * ## Only a person
 *
 * A protected class -- body camera, ALPR, tracker -- may be switched off by
 * the owner, because an explicit, visible choice is different in kind from a
 * baseline sweeping something up by accident. It must stay impossible for
 * anything *automatic* to do it: not a baseline, not the census, not a future
 * mesh peer. That is enforced by construction -- nothing but the console and
 * the glass calls the setter -- rather than by a flag, so if you are reading
 * this while adding a caller, that is the rule you are about to break.
 *
 * ## The stored value is the OFF set, deliberately
 *
 * Classes are appended to observore_class_t as the project learns to spot new
 * things -- fifteen so far. Storing which monitors are *on* would mean a mask
 * written today has a zero where tomorrow's class will be, and that class
 * would arrive switched off on every device that had ever saved a setting.
 * Storing which are *off* makes the default fall the safe way: an unknown bit
 * is zero, zero means on, and a missing or unreadable setting means
 * everything is on rather than nothing.
 */

/* Nothing disabled. Also what a missing or rejected setting resolves to. */
#define OBSERVORE_MONITORS_ALL_ON 0u

void observore_monitors_init(void);

/* Whether the device is reporting this class. Unknown is always "enabled":
 * it is the absence of a classification rather than a monitor. */
bool observore_monitors_enabled(observore_class_t cls);

/* Switch one monitor on or off. Returns false for a class that cannot be
 * toggled. Persisted, and only ever called for a deliberate human action. */
bool observore_monitors_set(observore_class_t cls, bool on);

/* Whether this class is something a person can switch off at all. */
bool observore_monitors_can_toggle(observore_class_t cls);

/* How many monitors are off. This is the number that has to travel with the
 * verdict: "clear" and "clear, 4 monitors off" are different claims. */
int observore_monitors_off_count(void);

/* The raw off-set, for persistence and for the console. */
uint32_t observore_monitors_off_mask(void);

/* Adopt an off-set read from storage. Bits for classes this build does not
 * have are dropped rather than refused -- a downgrade should lose a setting
 * it cannot represent, not every setting. Returns false if nothing usable
 * was in it, in which case everything stays on. */
bool observore_monitors_restore(uint32_t off_mask);
