#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "observore_types.h"

/* Detections that survive a reboot.
 *
 * Everything the device has seen lived only in RAM, so a power blip, a
 * firmware update or an unexplained restart erased the whole picture -- which
 * is why an unexplained reboot used to be indistinguishable from a quiet
 * night, on a device whose entire job is noticing what changed.
 *
 * This deliberately does NOT persist the device table. That table is working
 * state: 192 slots of mostly unclassified churn, rewritten constantly. What
 * has lasting value is the much smaller set of things that were actually
 * classified as something, with the times they were seen. Keeping only those
 * is what makes the write rate low enough to be safe for flash.
 */
#define OBSERVORE_HISTORY_MAX 48

typedef struct {
    uint8_t  mac[OBSERVORE_MAC_LEN];
    uint8_t  cls;                       /* observore_class_t */
    int8_t   rssi;                      /* strongest seen */
    char     label[OBSERVORE_LABEL_LEN];
    uint32_t hits;
    /* Wall-clock UTC, or 0 when the device never learned the time while this
     * entry was alive.  Filled in retroactively when the clock arrives. */
    int64_t  first_epoch;
    int64_t  last_epoch;
    /* Monotonic, this boot only; zero on an entry restored from flash, which
     * is how a reloaded entry is told apart from one seen since boot. */
    int64_t  first_us;
    int64_t  last_us;
} observore_history_entry_t;

void observore_history_init(void);

/* Record a classified detection.  Repeat sightings of something already known
 * update it in place rather than adding a row. */
void observore_history_note(const observore_event_t *ev);

/* Persist if there is anything worth persisting.  `force` bypasses the rate
 * limit; use it when the user is about to look, not on a timer. */
void observore_history_flush(bool force);

size_t observore_history_count(void);
size_t observore_history_copy(observore_history_entry_t *out, size_t max);

/* Forget everything recorded, in RAM and in flash. */
void observore_history_clear(void);
