#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Knowing whether a newer release exists, without acting on it.
 *
 * The check is deliberately separate from installing anything. It runs inside
 * an uplink window the device was going to open anyway, reads one small
 * document, and records what it found. Downloading is a separate, explicit act.
 */

void observore_update_init(void);

/* Read the manifest if it is time to. Cheap no-op otherwise, and a no-op
 * whenever the uplink is down, so it is safe to call from the main loop. */
void observore_update_check(void);

/* The version this image was built as, from the application descriptor. */
const char *observore_update_running_version(void);

/* The newest release seen, or an empty string if no check has succeeded. */
const char *observore_update_latest_version(void);

/* Whether the last successful check found something newer than what is
 * running. */
bool observore_update_available(void);

/* Why the last check failed, for the console. Empty when it did not. */
const char *observore_update_error(void);

/* Seconds since the last successful check, or -1 if there has not been one. */
long observore_update_age_s(void);
