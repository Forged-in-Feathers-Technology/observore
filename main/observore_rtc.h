#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* A clock that survives losing the network.
 *
 * Until now the time came only from SNTP, which means only from home: carry
 * the device somewhere, restart it, and it had no idea what time it was --
 * so findings recorded away from a network were undated, and a watch face
 * would have been blank. The PCF85063 on this board keeps time across a
 * reboot from its own oscillator.
 *
 * Read at startup to seed the system clock, and written whenever SNTP gives
 * a better answer, so the two correct each other in the direction that makes
 * sense: the network is more accurate, the chip is more available. */

void observore_rtc_init(void);

/* True where there is a chip answering. */
bool observore_rtc_available(void);

/* Seed the system clock from the chip. False if there is no chip or it says
 * its own contents are unreliable -- which it does after losing power, and
 * which is worth believing rather than reading anyway. */
bool observore_rtc_read(void);

/* Write the system clock into the chip. Called when SNTP lands. */
bool observore_rtc_write(void);

/* BCD, which the chip speaks and nothing else here does. Pure, so the host
 * tests cover the conversions rather than trusting them. */
uint8_t observore_bcd_to_dec(uint8_t bcd);
uint8_t observore_dec_to_bcd(uint8_t dec);

/* A broken-down UTC time to a Unix timestamp.
 *
 * timegm() is absent from this libc, and the obvious substitute -- setting TZ
 * to UTC around a mktime() call -- means mutating global state on a device
 * that has a timezone set for the person reading the screen. This is the
 * arithmetic instead: days since the epoch from a civil date, which is exact,
 * has no environment in it, and is covered by the host tests. */
time_t observore_timegm(const struct tm *tm);
