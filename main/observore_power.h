#pragma once

#include <stdbool.h>

/* Holding the board's own power on, where that is the firmware's job.
 *
 * The Waveshare 1.85 runs from USB without help, but its battery path is a
 * soft latch: the power button connects the cell only while it is held
 * down, and something has to assert a hold line before the button is
 * released or the board dies in your hand. Nothing did, so the board could
 * not run on battery at all -- press it and the screen came up, let go and
 * it went off again.
 *
 * Two lines make it work, and they are a pair:
 *
 *   BAT_Control  an output. Driven high, it latches the battery on. It has
 *                an external pull-down, so the default at reset is off --
 *                which is correct, because a board that latched itself on
 *                from a stray transient could never be switched off.
 *   Key_BAT      the button, as an input. Pulled high, grounded by a press.
 *
 * The hold has to be asserted early -- before a person lets go of a button
 * they have just pressed -- so it runs before anything else in app_main,
 * ahead of the radio, the panel and the I2C buses.
 *
 * And because firmware holds the power on, firmware has to be able to let
 * go: without that the only way to switch the board off would be to flatten
 * the cell or unplug it. A long press releases the latch.
 */

/* Latch the power on. First thing at boot, before anything slow. */
void observore_power_hold(void);

/* Watch the button for a long press and power off when it comes. */
void observore_power_start(void);

/* Whether the power button is down right now. */
bool observore_power_button(void);
