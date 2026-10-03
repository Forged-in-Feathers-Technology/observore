#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Turning contact into taps.
 *
 * A resistive panel reports pressure, not gestures. Deciding that a sequence
 * of pressure readings was one deliberate press is the job here, and it was
 * previously eight lines inside the touch task -- which meant the only way to
 * answer "how many taps did that one press produce" was a serial cable and a
 * finger. It is arithmetic over a sequence of samples, so it belongs where a
 * test can drive it.
 *
 * ## Why a release has to be held
 *
 * A press ends when the panel stops reporting contact. On the boards with the
 * controller's interrupt line wired up, "stops reporting contact" has two
 * independent witnesses: the pressure falls below the threshold *and* the
 * interrupt line goes high. On the NM-CYD-C5 the interrupt is not wired
 * anywhere -- the vendor's own pin table prints "---" for it -- so the
 * pressure reading is the only evidence there is.
 *
 * That makes the polled board structurally more fragile, not merely
 * differently configured: a single sub-threshold sample, 20 ms wide, is enough
 * to end a press. If the pressure dips once in the middle of a finger being
 * held on the glass, one press becomes two taps, and on a keyboard two taps
 * are two characters.
 *
 * So a release can be required to persist before it is believed. `release_us`
 * of zero restores the old behaviour exactly -- the first sample without
 * contact ends the press -- which is what the interrupt-gated boards get,
 * because they have a second witness and they work.
 *
 * ## What it does not do
 *
 * It does not merge two deliberate presses. The gap a person leaves between
 * two keystrokes is an order of magnitude longer than the gap this is
 * bridging, and the tests pin that down rather than assuming it.
 */

typedef struct {
    bool    down;          /* contact, as far as this machine is concerned */
    int     down_x;        /* where it went down: what a tap reports */
    int     down_y;
    int64_t down_us;       /* when it went down */
    int64_t lost_us;       /* when contact was first missing, 0 while held */
} observore_tap_state_t;

/* A contact shorter than this is electrical noise from the radio alongside
 * the panel rather than a finger. */
#define OBSERVORE_TAP_MIN_CONTACT_US (30 * 1000)

void observore_tap_reset(observore_tap_state_t *s);

/* Feed one sample.
 *
 * `down` is whether the panel reported contact, `x`/`y` where, `now_us` a
 * monotonic clock, and `release_us` how long a loss of contact must persist
 * before the press is considered over -- zero to end it on the first sample
 * without contact.
 *
 * Returns true when a tap has just completed, and then writes where the
 * finger went *down* to `*tap_x`/`*tap_y`: a thumb rolls a few pixels on the
 * way off the glass, and a button should not care.
 */
bool observore_tap_sample(observore_tap_state_t *s, bool down, int x, int y,
                          int64_t now_us, int64_t release_us,
                          int *tap_x, int *tap_y);
