#pragma once

#include "observore_track.h"

/* The XIAO ESP32S3 has one monochrome user LED, not an RGB pixel, so threat
 * level is encoded as a blink rhythm instead of a colour. */
void observore_led_init(void);
void observore_led_set_level(observore_level_t level);
void observore_led_set_console(bool console);
