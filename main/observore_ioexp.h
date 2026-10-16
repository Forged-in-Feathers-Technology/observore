#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The guards in this module's source test CONFIG_OBSERVORE_IOEXP, and an
 * undefined macro tests as zero -- which silently compiled the whole thing
 * away into its own do-nothing stub, on a board whose profile had the
 * option set. It ran, logged nothing, and the panel reset it exists to
 * drive would never have worked. Included here so no source of this
 * module can be compiled without it. */
#include "sdkconfig.h"

/* A TCA9554 between the chip and three things that need a pin.
 *
 * The Waveshare 1.85 has more peripherals than the S3 has convenient GPIOs, so
 * the panel's reset, the touch panel's reset and the SD card's chip select sit
 * on an eight-bit I2C expander instead.
 *
 * That makes the expander an ordering constraint rather than a detail: the
 * screen cannot be reset -- and therefore cannot be initialised -- until the
 * I2C bus it hangs off is up and this chip has been written to. A panel
 * brought up before that simply never answers, which looks exactly like the
 * wrong driver.
 *
 * ## Which bit is which is not assumed
 *
 * Waveshare's wiki numbers the lines EXIO1 upward and does not say whether
 * EXIO1 is the chip's P0 or its P1. That is the sort of thing this project has
 * been wrong about before, from a wiki that has already been wrong about this
 * vendor's touch pins, panel power and battery sense -- so the mapping lives
 * in the board profile as a bit number, and the number was settled by trying
 * it rather than by reading it.
 */

/* Whether the expander answered. False means every caller should carry on
 * without it rather than wait: a board with no expander is the normal case. */
bool observore_ioexp_present(void);

/* Bring it up: every line an output, all low. Call once, before anything that
 * needs a reset line. Safe to call on a board that has no expander. */
void observore_ioexp_init(void);

/* Drive one line. Bits are 0 to 7, as the chip numbers them. */
bool observore_ioexp_set(uint8_t bit, bool high);

/* A reset pulse on one line: low, wait, high. The usual shape, in one place,
 * so two callers cannot disagree about how long to hold it. */
bool observore_ioexp_reset_pulse(uint8_t bit, int low_ms, int settle_ms);
