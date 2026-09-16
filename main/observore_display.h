#pragma once

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
 * Compiled only for boards that have a panel; elsewhere both calls are empty. */

void observore_display_init(void);

/* Redraw what changed. Cheap when nothing did: each line is compared with what
 * is already on the glass and only a differing line is sent. `top` is the
 * classified devices, most recent first, as observore_track_snapshot() gives
 * them. */
void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us);
