#pragma once

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_I2C

#include "driver/i2c_master.h"

/* The one I2C bus, shared.
 *
 * Three chips sit on two wires on the round board -- the touch controller,
 * the motion sensor and a clock -- and the driver refuses to create a second
 * bus on the same port. Whoever needs it first creates it; everyone else
 * gets the same handle back. NULL means it could not be brought up, which
 * every caller is expected to survive: a board with no motion sensor is a
 * board that cannot say it is moving, not a board that stops working. */
i2c_master_bus_handle_t observore_i2c_bus(void);

/* Add a device to the shared bus, or NULL. */
i2c_master_dev_handle_t observore_i2c_device(uint8_t addr, uint32_t hz);

/* Read `len` bytes starting at `reg`.
 *
 * Two hundred milliseconds for two bytes looks absurd and is not: the
 * timeout covers the transaction completing, and on a board whose real work
 * is two radios that can sit behind them. At 50 ms every read in a poll loop
 * failed while the same read at startup, before the radios were busy,
 * succeeded. Measured, not chosen; a read that works returns in
 * microseconds. */
bool observore_i2c_read(i2c_master_dev_handle_t dev, uint8_t reg,
                        uint8_t *buf, size_t len);

/* Write one register. */
bool observore_i2c_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

/* Which addresses answer a one-byte read, as a printable list into `out`.
 *
 * Done with a read rather than i2c_master_probe(), which on this board
 * reports a timeout for every address including the three that answer a read
 * perfectly well. A scan that lies about an empty bus is worse than no scan:
 * it sends you to look at the wiring. */
void observore_i2c_scan(char *out, size_t len);

#endif
