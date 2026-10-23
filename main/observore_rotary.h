#pragma once

/* The rotary encoder on the CrowPanel 1.46, as a page selector.
 *
 * A knob is the one input this board has that the others do not, and the
 * thing a person standing at the device most often wants is the next page.
 * Touch already offers that; the knob offers it without having to look at
 * where a finger is landing, and without a fingerprint on the glass.
 *
 * Quadrature, read by the PCNT peripheral rather than by polling the two
 * pins. Two reasons, and the second is the one that matters:
 *
 *   A knob turned quickly outruns a poll. At the UI's poll interval a fast
 *   flick is several detents, and a software reader sees one of them -- or,
 *   worse, sees the two pins in an order that never happened and counts a
 *   detent backwards.
 *
 *   PCNT counts in hardware, continuously, whatever the CPU is doing. The
 *   UI loop then asks what the total is whenever it gets round to it, and
 *   the answer is right regardless of how late it asked.
 *
 * Off unless a board profile turns it on.
 */

void observore_rotary_init(void);

/* Detents turned since the last call: positive clockwise, negative anti.
 * Zero when the knob has not moved, or when there is no knob.
 *
 * Consuming, so two callers would each see part of the motion. There is
 * one caller, the UI loop. */
int observore_rotary_take(void);
