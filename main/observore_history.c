#include "observore_history.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "observore_clock.h"
#include "observore_nvs.h"

static const char *TAG = "observore.history";

/* How rarely this is allowed to write, and why it is allowed to at all.
 *
 * Flash wear is the whole design constraint here. NVS is 24 KB and a careless
 * baseline once pushed ~390 KB through it, so "write the table periodically"
 * was never an option.
 *
 * Two kinds of change are distinguished. A *structural* change -- a device
 * classified for the first time, or an entry evicted -- is the thing worth
 * keeping, and is rate limited rather than immediate. A *soft* change, meaning
 * another sighting of something already recorded, only moves a counter and a
 * timestamp; it never triggers a write on its own and rides along with the
 * next one that happens for another reason.
 *
 * The effect is that the write rate tracks how many genuinely new things the
 * device sees, which in a baselined deployment is close to zero -- and in a
 * noisy one is still bounded by the interval below. */
#define FLUSH_MIN_INTERVAL_US (5 * 60 * 1000000LL)

/* How long to let the clock arrive before persisting anything undated.
 *
 * Writing too early is its own kind of data loss. Dating is retroactive only
 * while the monotonic timestamps survive, and those die with the boot -- so an
 * entry flushed before the time is known, on a device that then restarts, is
 * undatable for ever. Detections in the first seconds after power-on are
 * exactly the ones at risk, because the clock does not arrive until the uplink
 * does.
 *
 * So a write is held back while the clock is unset, but only for a while: a
 * device with no network never learns the time at all, and there an undated
 * record still beats no record. */
#define CLOCK_GRACE_US (5 * 60 * 1000000LL)

static observore_history_entry_t s_hist[OBSERVORE_HISTORY_MAX];
static size_t  s_count;
static bool    s_structural;         /* a write is warranted */
static bool    s_soft;               /* counts moved; ride along */
static int64_t s_last_write_us;

static void backfill_epochs(void)
{
    if (!observore_clock_valid()) {
        return;
    }
    /* An entry first seen before the clock was set has no date yet. The
     * conversion is retroactive, so the moment the time arrives every such
     * entry can be dated correctly rather than staying blank for ever. */
    for (size_t i = 0; i < s_count; i++) {
        observore_history_entry_t *e = &s_hist[i];
        if (e->first_epoch == 0 && e->first_us != 0) {
            e->first_epoch = observore_clock_at(e->first_us);
        }
        if (e->last_us != 0) {
            e->last_epoch = observore_clock_at(e->last_us);
        }
    }
}

void observore_history_init(void)
{
    observore_nvs_item_t item = {.key = "history", .type = OBSERVORE_NVS_BLOB,
                                 .buf = s_hist, .len = sizeof(s_hist)};
    observore_nvs_read(&item, 1);
    if (!item.found) {
        s_count = 0;
        ESP_LOGI(TAG, "no stored history");
        return;
    }

    /* A short read means a build with a smaller table wrote it; take whatever
     * whole entries are there rather than discarding the lot. */
    size_t stored = item.len / sizeof(s_hist[0]);
    s_count = stored > OBSERVORE_HISTORY_MAX ? OBSERVORE_HISTORY_MAX : stored;

    size_t live = 0;
    for (size_t i = 0; i < s_count; i++) {
        /* Restored entries belong to a previous boot, so their monotonic
         * timestamps mean nothing in this one. Clearing them is what stops a
         * reloaded row being reported as "seen 3 seconds ago". */
        s_hist[i].first_us = 0;
        s_hist[i].last_us = 0;
        if (s_hist[i].cls != OBSERVORE_CLASS_UNKNOWN) {
            live++;
        }
    }
    ESP_LOGI(TAG, "%zu detection%s restored from before the last restart",
             live, live == 1 ? "" : "s");
}

void observore_history_note(const observore_event_t *ev)
{
    if (!ev || ev->cls == OBSERVORE_CLASS_UNKNOWN) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t wall = observore_clock_valid() ? observore_clock_at(now) : 0;

    /* Same address and same classification is the same thing seen again, not
     * a new row -- otherwise one persistent beacon would evict everything
     * else it is meant to be remembered alongside. */
    for (size_t i = 0; i < s_count; i++) {
        observore_history_entry_t *e = &s_hist[i];
        if (e->cls == ev->cls && memcmp(e->mac, ev->mac, OBSERVORE_MAC_LEN) == 0) {
            e->hits++;
            e->last_us = now;
            if (wall) {
                e->last_epoch = wall;
            }
            if (ev->rssi > e->rssi) {
                e->rssi = ev->rssi;
            }
            s_soft = true;
            return;
        }
    }

    observore_history_entry_t *e;
    if (s_count < OBSERVORE_HISTORY_MAX) {
        e = &s_hist[s_count++];
    } else {
        /* Full: drop the least recently seen. Oldest-first rather than
         * newest-first, because the question this answers is "what has been
         * around lately", and an entry nothing has refreshed in weeks is the
         * one worth losing. */
        size_t oldest = 0;
        for (size_t i = 1; i < s_count; i++) {
            if (s_hist[i].last_epoch < s_hist[oldest].last_epoch) {
                oldest = i;
            }
        }
        e = &s_hist[oldest];
    }

    memset(e, 0, sizeof(*e));
    memcpy(e->mac, ev->mac, OBSERVORE_MAC_LEN);
    e->cls   = (uint8_t)ev->cls;
    e->rssi  = ev->rssi;
    e->hits  = 1;
    e->first_us = e->last_us = now;
    e->first_epoch = e->last_epoch = wall;
    snprintf(e->label, sizeof(e->label), "%s", ev->label);

    s_structural = true;
}

void observore_history_flush(bool force)
{
    if (!s_structural && !(force && s_soft)) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!force && s_last_write_us != 0 &&
        now - s_last_write_us < FLUSH_MIN_INTERVAL_US) {
        return;                       /* rate limited; the change is not lost */
    }
    if (!observore_clock_valid() && now < CLOCK_GRACE_US) {
        return;                       /* give the time a chance to arrive */
    }

    backfill_epochs();

    const observore_nvs_item_t item = {
        .key = "history", .type = OBSERVORE_NVS_BLOB, .buf = s_hist,
        .len = s_count * sizeof(s_hist[0])};
    observore_nvs_write(&item, 1);

    s_last_write_us = now;
    s_structural = false;
    s_soft = false;
    ESP_LOGD(TAG, "history written (%zu entries)", s_count);
}

size_t observore_history_count(void)
{
    return s_count;
}

size_t observore_history_copy(observore_history_entry_t *out, size_t max)
{
    size_t n = s_count < max ? s_count : max;
    backfill_epochs();
    memcpy(out, s_hist, n * sizeof(*out));
    return n;
}

void observore_history_clear(void)
{
    memset(s_hist, 0, sizeof(s_hist));
    s_count = 0;
    s_structural = false;
    s_soft = false;
    const observore_nvs_item_t item = {
        .key = "history", .type = OBSERVORE_NVS_BLOB, .buf = s_hist, .len = 0};
    observore_nvs_write(&item, 1);
    ESP_LOGI(TAG, "history cleared");
}
