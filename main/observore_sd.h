#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The MicroSD slot, where a board has one.
 *
 * Only the Waveshare 1.85 does, and it is wired awkwardly: the card's clock,
 * MISO and MOSI are ordinary pins, but its chip select is a line on the
 * TCA9554 I/O expander rather than a GPIO. ESP-IDF's SPI driver asserts chip
 * select itself, in hardware, around each transaction -- it cannot drive a
 * pin that is on the far side of an I2C bus.
 *
 * What makes that survivable is that nothing else is on those three wires.
 * The panel is on its own QSPI bus; this one carries the card alone. So the
 * select can be asserted once at mount and simply left low, with the driver
 * told there is no chip select to manage. A shared bus would need the line
 * toggled per transaction over I2C, which at a few hundred microseconds a
 * time would be slower than the transfers it was gating.
 *
 * Why bother: the detection history lives in NVS and holds 48 entries. A
 * device that watches a place for a week has far more to say than that, and
 * the limit is storage rather than anything about the detector.
 */

void observore_sd_init(void);

/* Whether a card is mounted and writable. */
bool observore_sd_ready(void);

/* Capacity and free space in megabytes, or zeros when no card is mounted. */
void observore_sd_space(uint32_t *total_mb, uint32_t *free_mb);

/* What the card calls itself, for the system page. Empty when none. */
const char *observore_sd_name(void);
