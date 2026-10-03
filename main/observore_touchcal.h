#pragma once

#include <stdbool.h>

/* Turning two presses into the bounds of a resistive sheet.
 *
 * The device shows a target near one corner and then near the other, reads
 * the controller's raw channels at each, and has to work out what the whole
 * sheet would read corner to corner. Two things make that more than a
 * subtraction, and the first version got the second one wrong.
 *
 * ## The targets are inset, so the span has to be scaled out
 *
 * They are not in the corners: the bezel overlaps the glass on these boards
 * and a press right at the edge often does not register at all. So the two
 * presses span only part of the sheet, and the measured span is scaled up by
 * the ratio of the full screen to the distance between the targets. Without
 * that, every press lands slightly towards the middle.
 *
 * ## The controller's axes need not be the screen's
 *
 * On every resistive board here the panel is wired so that the controller's X
 * channel runs along the screen's *vertical* axis (CONFIG_OBSERVORE_TOUCH_SWAP_XY).
 * The scaling above therefore has to pair each raw channel with the screen
 * axis it actually travels along -- raw X with the screen's height, raw Y with
 * its width.
 *
 * The first version of this paired raw X with the screen's width regardless.
 * On a 320x240 panel with a two-cell inset that scaled one axis by 1.11 where
 * it needed 1.40 and the other by 1.40 where it needed 1.11: both axes wrong,
 * in opposite directions, which is why it read as the whole alignment being
 * off rather than as one edge being short. It affected all four boards the
 * calibration runs on, because all four swap.
 *
 * Hence this, out here, where a test can build a synthetic panel and check
 * that the bounds it recovers are the bounds it started from.
 */

typedef struct {
    int lo_x, hi_x;
    int lo_y, hi_y;
} observore_touchcal_t;

/* The smallest raw span worth believing. Below this the two presses were
 * effectively in the same place -- a panel answering with a stuck value, or
 * somebody pressing the same spot twice -- and the old bounds are better than
 * a mapping derived from noise. */
#define OBSERVORE_TOUCHCAL_MIN_SPAN 100

/* Solve for the sheet's bounds.
 *
 * `raw_*0` and `raw_*1` are the controller's readings at the two targets, and
 * `sx0,sy0` / `sx1,sy1` the screen pixels those targets were drawn at.
 * `disp_w`/`disp_h` are the panel's size, and `swap` says the controller's X
 * channel runs along the screen's Y -- the same flag the mapping uses, passed
 * in rather than read here so this stays testable in both arrangements.
 *
 * Returns false and leaves `*out` alone when the presses cannot describe a
 * sheet: the caller keeps whatever bounds it had, which is always better than
 * installing a mapping that is known to be wrong.
 */
bool observore_touchcal_solve(int raw_x0, int raw_y0, int raw_x1, int raw_y1,
                              int sx0, int sy0, int sx1, int sy1,
                              int disp_w, int disp_h, bool swap,
                              observore_touchcal_t *out);

/* Fit bounds to what the controller is physically able to report, or reject
 * them as something it could not have produced. `raw_max` is the largest
 * reading the converter can return -- 4095 for the XPT2046's twelve bits.
 *
 * An endpoint outside the range is ordinary rather than broken.
 * Extrapolating from inset targets assumes the sheet is linear, and near the
 * bezel it is not quite, so a correct calibration can land a few counts past
 * either end. Those are clamped, because the sheet does stop there.
 *
 * What it does NOT do is decide whether a calibration came from sound
 * arithmetic. That was the first design -- recognise the bounds the bug fixed
 * in #149 produced, by their being impossible -- and the tests said it cannot
 * work: a real calibration that overshoots both ends has a span wider than the
 * converter too, one bench board's bad bounds were only 136 counts over, and
 * on a panel with a narrower usable range the same bug lands entirely inside
 * the valid range. Provenance is recorded in the stored calibration instead;
 * this is only about fitting numbers to hardware.
 *
 * Clamping can only narrow a span, so the floor is re-checked after it:
 * bounds that were mostly outside the range come back as a sliver, and a
 * sliver is not a calibration.
 *
 * Returns false when the bounds are unusable, and leaves `*c` alone.
 */
bool observore_touchcal_fit(observore_touchcal_t *c, int raw_max);
