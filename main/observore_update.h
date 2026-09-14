#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

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

/* Installing. Separate from the check on purpose: the device notices a release
 * on its own, and only ever downloads one because somebody asked it to. */

typedef enum {
    OBSERVORE_UPDATE_IDLE = 0,
    OBSERVORE_UPDATE_REQUESTED,   /* asked for, waiting for an uplink window */
    OBSERVORE_UPDATE_RUNNING,
    OBSERVORE_UPDATE_FAILED,
    OBSERVORE_UPDATE_REBOOTING,
} observore_update_state_t;

/* Ask for the available update to be installed. Returns an error, and changes
 * nothing, when there is nothing to install or the device cannot know which
 * image is its own. Safe to call from the web task: the work happens in the
 * main loop. */
esp_err_t observore_update_install(void);

observore_update_state_t observore_update_state(void);
const char *observore_update_state_name(observore_update_state_t s);
/* Percent of the image written, or -1 when the size is not yet known. */
int observore_update_progress(void);

/* Run a requested install. Blocks for the whole download, which is the point:
 * holding the main loop keeps the uplink window from being torn down under it.
 * No-op unless an install was asked for. */
void observore_update_service(void);

/* Confirm the running image after it has proved itself, cancelling the
 * rollback the bootloader is holding. No-op on an image that is not on
 * probation. */
void observore_update_confirm(void);
