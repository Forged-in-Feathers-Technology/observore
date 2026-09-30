#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The cell, on the one board that can measure its own.
 *
 * None of the Cheap Yellow Displays can: their battery connector charges a
 * cell and reports nothing, which is why "dim on battery, bright on USB" was
 * never possible there. This board divides the cell by three into ADC1
 * channel 3 -- GPIO4 -- and that is measurable.
 *
 * Read off the vendor's own example rather than the wiki, which said GPIO5.
 * The wiki has now been wrong about this board's touch pins, its panel power
 * and its battery sense; its own source has been right every time. */

void observore_battery_init(void);

/* Millivolts at the cell, or -1 where there is no sense line or no reading
 * yet. Note that on USB this reads the charger's output rather than a
 * discharging cell, so a number near full while plugged in says little. */
int observore_battery_mv(void);

/* Rough charge, 0-100, or -1 when unknown.
 *
 * A lithium cell's voltage is not linear in its charge, so this is a small
 * curve rather than a straight line between two numbers -- the flat middle of
 * the discharge is where a linear guess is most wrong, and it is where a
 * battery spends most of its life. Pure arithmetic, so the host tests cover
 * it. */
int observore_battery_pct_from_mv(int mv);

/* False on every board without a sense line. */
bool observore_battery_available(void);
