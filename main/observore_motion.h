#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Whether the device is being carried, and how many journeys it has made.
 *
 * This is the only sensor here that measures the device rather than the room,
 * and it exists to answer one question the radio cannot: a device that stays
 * with you *across movement* is following you, where one that persists while
 * this board sits on a desk is the neighbourhood. See OBSERVORE_CLASS_TAILING.
 *
 * Orientation is deliberately not exposed. Which way up the thing is tells
 * you nothing about who is nearby, and a screen that rotates is a screen that
 * draws when nobody asked it to. */

void observore_motion_init(void);

/* True while the board is being carried rather than resting. */
bool observore_motion_moving(void);

/* How many journeys have finished since boot.
 *
 * A journey is sustained movement followed by settling: picked up, carried
 * somewhere, put down. The counter is what the tracker compares against,
 * because "this device was here before I travelled and is here after" is a
 * claim about crossing that boundary, not about the instant of moving. */
uint32_t observore_motion_journeys(void);

/* False where there is no sensor, which is every board but one. */
bool observore_motion_available(void);
