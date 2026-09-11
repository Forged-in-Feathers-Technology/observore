#pragma once

#include "argus_track.h"

/* The XIAO ESP32S3 has one monochrome user LED, not an RGB pixel, so threat
 * level is encoded as a blink rhythm instead of a colour. */
void argus_led_init(void);
void argus_led_set_level(argus_level_t level);
void argus_led_set_console(bool console);
