#pragma once

#include "observore_track.h"

/* Threat level always reaches the eye as a blink rhythm, because the XIAO
 * ESP32S3 has a single monochrome LED and that is the lowest common
 * denominator.  Boards with an addressable pixel -- the ESP32-C5 kits -- carry
 * the same rhythm and add colour on top of it.  Which backend is compiled in
 * is a Kconfig choice, not a target check: the two are not the same axis. */
void observore_led_init(void);
void observore_led_set_level(observore_level_t level);
void observore_led_set_console(bool console);
