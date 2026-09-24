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

/* True once per press, on release, with the position where the finger went
 * down. A tap is the gesture a button wants: it cannot be held down to repeat
 * by accident, and it is decided after the finger has settled. */
bool observore_touch_tap(int *x, int *y);
