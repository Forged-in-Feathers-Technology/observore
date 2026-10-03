#pragma once

#include <stdbool.h>

/* The resistive touch panel on a Cheap Yellow Display.
 *
 * An XPT2046 on its own SPI pins, separate from the panel's, so the two never
 * contend. Reads are gated on the controller's PENIRQ line: no touch, no SPI
 * traffic at all, which matters on a device whose real work is listening.
 *
 * Coordinates come out in screen space -- the same pixels the display draws
 * in, after the same rotation and mirroring -- so a caller never sees a raw
 * ADC count. Compiled only where a panel and touch are configured. */

void observore_touch_init(void);

/* True while a finger is down, with the position in screen pixels. Debounced
 * and median-filtered; a read the driver does not trust reports nothing
 * rather than a wrong position. */
bool observore_touch_read(int *x, int *y);

/* The raw reading under the finger, before any mapping: the two numbers a
 * calibration needs and nothing else does. False when nothing is pressed.
 *
 * The edges of a resistive sheet differ from panel to panel, not just from
 * model to model, so the bounds compiled in are one bench unit's and no more
 * than a sensible start. A board whose own sheet reads differently puts
 * presses in the wrong row, and until there was a way to measure it in the
 * field the only fix was rebuilding the firmware. */
bool observore_touch_raw(int *x, int *y);

/* Replace the mapping bounds and write them down, so the panel in front of
 * somebody beats the one that happened to be on my bench. */
void observore_touch_set_bounds(int min_x, int max_x, int min_y, int max_y);

/* What is in use now, whether measured here or compiled in. */
void observore_touch_bounds(int *min_x, int *max_x, int *min_y, int *max_y);

/* True when the bounds came from a calibration on this device. */
bool observore_touch_calibrated(void);

/* True once per press, on release, with the position where the finger went
 * down. A tap is the gesture a button wants: it cannot be held down to repeat
 * by accident, and it is decided after the finger has settled. */
bool observore_touch_tap(int *x, int *y);
