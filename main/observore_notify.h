#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "observore_track.h"

#ifndef OBSERVORE_HOST_TEST
#include "esp_err.h"
#endif

/* Push notifications to a Gotify server.
 *
 * Detections happen in patrol mode, which has no network, so notices are
 * queued and flushed the next time the uplink is up rather than being dropped.
 * The queue is small and bounded: this is a notifier, not a store, and the
 * device table is the real record.
 *
 * The token is stored in NVS and, like the Wi-Fi password, is write-only from
 * outside the device. */

#define OBSERVORE_NOTIFY_URL_LEN   128
#define OBSERVORE_NOTIFY_TOKEN_LEN 64
#define OBSERVORE_NOTIFY_QUEUE     24
#define OBSERVORE_NOTIFY_TITLE_LEN 48
#define OBSERVORE_NOTIFY_MSG_LEN   160

void observore_notify_init(void);

bool observore_notify_configured(void);
/* Copies the server URL only.  There is no way to read the token back. */
bool observore_notify_url(char *out, size_t len);
esp_err_t observore_notify_set(const char *url, const char *token);
esp_err_t observore_notify_clear(void);

/* Queue a notice about a newly identified device. */
void observore_notify_event(const observore_event_t *ev);
/* Queue a notice that the threat level rose.  Falls are not reported: an
 * alert that clears is not news, and reporting it doubles the traffic. */
void observore_notify_level(observore_level_t from, observore_level_t to, uint16_t score);

/* Send whatever is queued, if the uplink is up.  Cheap no-op otherwise. */
void observore_notify_pump(void);

/* Send one message immediately, for the console's Test button.  Returns the
 * transport error so the UI can say what actually went wrong. */
esp_err_t observore_notify_test(void);

uint32_t observore_notify_sent(void);
uint32_t observore_notify_failed(void);
uint32_t observore_notify_dropped(void);
size_t   observore_notify_pending(void);
/* Last transport error, or an empty string. */
const char *observore_notify_last_error(void);
