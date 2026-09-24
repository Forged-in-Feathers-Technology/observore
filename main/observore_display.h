#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "observore_track.h"

/* A screen for a device that has one.
 *
 * It shows what the device sees and nothing about how to get into it: the
 * level as colour, the score, the top findings one per line, and how long it
 * has been up. The console password is never drawn -- a screen faces a room,
 * and the device already prints the password to serial for whoever is setting
 * it up. Nothing on the screen is gated for the same reason: the glass on a
 * desk is a personal display, and the authentication belongs to the network.
 *
 * Where the board also has a touch panel the screen gains a button bar and a
 * second page; the rules above do not change, because a person who can touch
 * the glass is already standing in front of it.
 *
 * Compiled only for boards that have a panel; elsewhere the calls are empty. */

/* The panel in landscape, in pixels. Shared because the touch layer has to
 * land in the same coordinate space the display draws in. */
#define OBSERVORE_DISPLAY_W 320
#define OBSERVORE_DISPLAY_H 240

void observore_display_init(void);

/* Redraw what changed. Cheap when nothing did: each line is compared with what
 * is already on the glass and only a differing line is sent. `top` is the
 * classified devices, most recent first, as observore_track_snapshot() gives
 * them. */
void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us);

/* Show one line, in the attention colour, for a few seconds. For the result
 * of something the person standing at the device just did. */
void observore_display_notice(const char *text, int seconds);

/* Hold the backlight steady while something sensitive happens next to it.
 *
 * The backlight is driven by a PWM whose switching lands inside the touch
 * controller's measuring band: with it running, the X axis reads a constant
 * near its top of scale no matter where the glass is pressed, which looks
 * exactly like a dead axis. Sampling is already gated on the controller's
 * interrupt line, so the pause only ever happens with a finger on the screen,
 * for about a millisecond, which nobody can see.
 *
 * Calls nest: resume restores the level only when the last hold is released. */
void observore_display_backlight_hold(void);
void observore_display_backlight_release(void);

/* True once when the screen's Baseline button has been pressed. Polled by the
 * main loop, which owns the memory a baseline needs; the same request the
 * physical button makes. */
bool observore_display_take_baseline_request(void);
