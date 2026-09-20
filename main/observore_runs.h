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
    uint32_t up_s;   /* how long it ran, to within the write interval */
    uint8_t  end;    /* esp_reset_reason_t: how it ended */
} observore_run_t;

/* Reads the record and files the previous run. Call after NVS is up. */
void observore_runs_init(void);

/* Writes the current uptime when the interval is due. Safe from the main
 * loop; a cheap no-op between writes. */
void observore_runs_tick(void);

/* Completed runs, newest first. Returns how many were written. */
size_t observore_runs_list(observore_run_t *out, size_t cap);

/* The word for a reset reason, shared with the console. */
const char *observore_reset_reason_name(esp_reset_reason_t r);
