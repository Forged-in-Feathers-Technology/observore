#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_system.h"

/* How long each run lasted and how it ended.
 *
 * A device left on battery overnight comes back with an uptime of seven hours
 * and a reset reason, and that is all: the run before it is gone. The battery
 * question -- how long does it actually last? -- can only be answered from
 * the runs that ended, so the current uptime is written to flash every few
 * minutes, and at boot the last value written becomes the length of the run
 * that just ended, filed with the reason this boot happened. */

#define OBSERVORE_RUNS_MAX 8

typedef struct {
    uint32_t up_s;      /* how long it ran, to within the write interval */
    uint8_t  end;       /* esp_reset_reason_t: how it ended */
    uint16_t mv_start;  /* cell millivolts when it began, 0 if not measured */
    uint16_t mv_end;    /* cell millivolts at its last write, 0 if not measured */
} observore_run_t;

/* Reads the record and files the previous run. Call after NVS is up. */
void observore_runs_init(void);

/* Writes the current uptime when the interval is due. Safe from the main
 * loop; a cheap no-op between writes. */
void observore_runs_tick(void);

/* The cell voltage, pushed in from the main loop.
 *
 * Pushed rather than read, for the same reason the journey count and the
 * census day are: the record then depends on a number instead of on a sensor,
 * and the ordering problem goes away -- this record is restored from flash
 * before the battery ADC is configured, so there is nothing to sample at
 * init.
 *
 * The first reading of a run is kept as its start and never replaced; every
 * later one replaces its end. So the record says what the cell was when the
 * run began and what it was when it stopped, and that is enough on its own
 * (#165):
 *
 *   4.2 V -> 3.2 V   a battery run that went the distance
 *   4.2 V -> 4.2 V   plugged in throughout
 *   4.2 V -> 3.9 V   a short battery run, or a long one on a tiring cell
 *
 * No transition detection, which is the part worth avoiding. A full cell on
 * USB looks much like a full cell on battery, and a weak USB supply that
 * cannot hold the cell up looks exactly like being unplugged -- this project
 * has already met that one, on a laptop port that could not supply charge
 * current and the Wi-Fi radio's turn-on surge together.
 *
 * Zero means "not measured", not "flat". Only the round AMOLED board can
 * measure its own supply; the others pass zero and the record stays silent
 * rather than reporting an empty cell. */
void observore_runs_note_mv(uint16_t mv);

/* Completed runs, newest first. Returns how many were written. */
size_t observore_runs_list(observore_run_t *out, size_t cap);

/* The word for a reset reason, shared with the console. */
const char *observore_reset_reason_name(esp_reset_reason_t r);
