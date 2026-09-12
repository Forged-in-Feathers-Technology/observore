#include <stdio.h>
#include <string.h>

#include "observore_mute.h"
#include "observore_track.h"

#ifdef OBSERVORE_HOST_TEST
#define OBSERVORE_LOCK()   do {} while (0)
#define OBSERVORE_UNLOCK() do {} while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
#define OBSERVORE_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define OBSERVORE_UNLOCK() xSemaphoreGiveRecursive(s_lock)
#endif

typedef struct {
    observore_event_t ev;
    int64_t       last_scored_us;
    bool          in_use;
    bool          classified;
    bool          has_scored;   /* distinct from last_scored_us == 0, which is
                                 * a legitimate timestamp at boot */
    bool          reported;     /* already emitted to the serial event log */
} observore_slot_t;

static observore_slot_t s_devices[OBSERVORE_MAX_DEVICES];
static uint16_t     s_score;
static int64_t      s_last_decay_us;
static uint32_t     s_total_sightings;

void observore_track_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    observore_track_clear();
}

void observore_track_clear(void)
{
    OBSERVORE_LOCK();
    memset(s_devices, 0, sizeof(s_devices));
    s_score = 0;
    s_last_decay_us = 0;
    s_total_sightings = 0;
    OBSERVORE_UNLOCK();
}

static observore_slot_t *find_slot(const uint8_t mac[OBSERVORE_MAC_LEN])
{
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            memcmp(s_devices[i].ev.mac, mac, OBSERVORE_MAC_LEN) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/* Claim a slot, evicting the least recently seen entry when the table is full.
 * Classified devices are never evicted in favour of an unclassified one --
 * losing a confirmed bodycam to make room for a passing phone would be the
 * wrong trade. */
static observore_slot_t *claim_slot(void)
{
    observore_slot_t *victim = NULL;
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            return &s_devices[i];
        }
        if (s_devices[i].classified) {
            continue;
        }
        if (!victim || s_devices[i].ev.last_seen_us < victim->ev.last_seen_us) {
            victim = &s_devices[i];
        }
    }
    if (victim) {
        return victim;
    }
    /* Every slot holds a classified device.  Evict the oldest of those. */
    victim = &s_devices[0];
    for (size_t i = 1; i < OBSERVORE_MAX_DEVICES; i++) {
        if (s_devices[i].ev.last_seen_us < victim->ev.last_seen_us) {
            victim = &s_devices[i];
        }
    }
    return victim;
}

/* Decay is applied lazily from the last accounted-for instant rather than on a
 * timer tick, so the score is the same whether tick() runs every second or not
 * at all.  Every public entry point calls this first. */
static void apply_decay(int64_t now_us)
{
    if (s_score == 0) {
        s_last_decay_us = now_us;
        return;
    }
    while (s_score > 0 &&
           now_us - s_last_decay_us >= OBSERVORE_SCORE_DECAY_INTERVAL_US) {
        s_score--;
        s_last_decay_us += OBSERVORE_SCORE_DECAY_INTERVAL_US;
    }
    /* Once bottomed out, stop carrying decay debt forward -- otherwise a later
     * burst would be decayed away the instant it arrived. */
    if (s_score == 0) {
        s_last_decay_us = now_us;
    }
}

/* Add points for a device, respecting the per-device cooldown so a beacon
 * shouting ten times a second cannot run the score away on its own. */
static void score_device(observore_slot_t *slot, int64_t now_us)
{
    if (slot->ev.points == 0) {
        return;
    }
    if (slot->has_scored &&
        now_us - slot->last_scored_us < OBSERVORE_SCORE_COOLDOWN_US) {
        return;
    }
    slot->has_scored = true;
    slot->last_scored_us = now_us;
    uint32_t next = (uint32_t)s_score + slot->ev.points;
    s_score = (next > OBSERVORE_SCORE_MAX) ? OBSERVORE_SCORE_MAX : (uint16_t)next;
}

bool observore_track_observe(const observore_observation_t *obs, int64_t now_us)
{
    if (!obs || !obs->mac) {
        return false;
    }
    if (obs->rssi != 0 && obs->rssi < OBSERVORE_RSSI_FLOOR) {
        return false;
    }

    /* The name and the advert fingerprint are what survive a MAC rotation, so
     * both are computed before the mute check rather than after. */
    char name[OBSERVORE_LABEL_LEN + 8] = {0};
    if (obs->src == OBSERVORE_SRC_BLE) {
        observore_adv_name(obs->adv, obs->adv_len, name, sizeof(name));
    } else if (obs->ssid) {
        snprintf(name, sizeof(name), "%s", obs->ssid);
    }
    uint32_t fingerprint = (obs->src == OBSERVORE_SRC_BLE)
                               ? observore_fingerprint(obs->adv, obs->adv_len)
                               : 0;

    observore_event_t classified;
    bool is_classified = observore_classify(obs, &classified);

    /* Suppress before the table is touched, not after.  A muted device that
     * still occupied a slot would keep being promoted by the follower
     * heuristic and keep evicting things you do care about. */
    if (observore_mute_matches(obs->mac,
                           is_classified ? classified.cls : OBSERVORE_CLASS_UNKNOWN,
                           name[0] ? name : NULL, fingerprint)) {
        return false;
    }

    OBSERVORE_LOCK();
    apply_decay(now_us);
    s_total_sightings++;

    observore_slot_t *slot = find_slot(obs->mac);
    if (!slot) {
        slot = claim_slot();
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        memcpy(slot->ev.mac, obs->mac, OBSERVORE_MAC_LEN);
        slot->ev.first_seen_us = now_us;
        /* Name the vendor once, for every device -- this is what lets an
         * unrecognised MAC be read as "my own handset" and muted, and what
         * gives a follower hit something to go on. */
        slot->ev.addr_random = observore_obs_is_random(obs);
        slot->ev.fingerprint = fingerprint;
        slot->ev.vendor = slot->ev.addr_random ? NULL
                                               : observore_vendor_lookup(obs->mac);
    }

    slot->ev.hits++;
    slot->ev.last_seen_us = now_us;
    slot->ev.src = obs->src;
    slot->ev.channel = obs->channel;
    /* Keep the strongest RSSI seen: it is the closest approach, which is the
     * number that matters when deciding whether something is on you. */
    if (obs->rssi != 0 && (slot->ev.rssi == 0 || obs->rssi > slot->ev.rssi)) {
        slot->ev.rssi = obs->rssi;
    }

    /* Learn the device's own name for everything, not just for things that
     * classified -- an unidentified box called "Living Room TV" is far easier
     * to recognise and ignore than a bare MAC with a vendor beside it.
     *
     * Sticky once learned: BLE names frequently arrive in a scan response
     * rather than the first advert, so the name may show up several sightings
     * after the device does, and must not then be overwritten by a later
     * nameless advert from the same address. */
    if (slot->ev.detail[0] == '\0' && name[0]) {
        snprintf(slot->ev.detail, sizeof(slot->ev.detail), "%s", name);
    }
    /* A fingerprint can arrive late for the same reason a name can. */
    if (slot->ev.fingerprint == 0 && fingerprint != 0) {
        slot->ev.fingerprint = fingerprint;
    }

    if (is_classified) {
        slot->ev.cls = classified.cls;
        slot->ev.evidence = classified.evidence;
        slot->ev.points = classified.points;
        memcpy(slot->ev.label, classified.label, sizeof(slot->ev.label));
        if (classified.detail[0]) {
            memcpy(slot->ev.detail, classified.detail, sizeof(slot->ev.detail));
        }
        slot->classified = true;
    } else if (!slot->classified && obs->src == OBSERVORE_SRC_BLE) {
        /* The follower heuristic.  This is the piece that catches hardware
         * with no signature at all -- the reason to build the thing. */
        int64_t span = slot->ev.last_seen_us - slot->ev.first_seen_us;
        if (slot->ev.hits >= OBSERVORE_FOLLOWER_MIN_HITS &&
            span >= OBSERVORE_FOLLOWER_MIN_SPAN_US) {
            slot->ev.cls = OBSERVORE_CLASS_FOLLOWER;
            slot->ev.evidence = OBSERVORE_EVIDENCE_PERSISTENCE;
            slot->ev.points = observore_class_points(OBSERVORE_CLASS_FOLLOWER);
            snprintf(slot->ev.label, sizeof(slot->ev.label), "persistent %s",
                     slot->ev.vendor        ? slot->ev.vendor
                     : slot->ev.addr_random ? "(random MAC)"
                                            : "device");
            slot->classified = true;
        }
    }

    bool reportable = slot->classified;
    if (reportable) {
        score_device(slot, now_us);
    }
    OBSERVORE_UNLOCK();
    return reportable;
}

void observore_track_tick(int64_t now_us)
{
    OBSERVORE_LOCK();
    apply_decay(now_us);

    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            now_us - s_devices[i].ev.last_seen_us > OBSERVORE_DEVICE_TTL_US) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
        }
    }
    OBSERVORE_UNLOCK();
}

static observore_level_t level_for(uint16_t score)
{
    if (score >= 6) {
        return OBSERVORE_LEVEL_ALERT;
    }
    if (score >= 3) {
        return OBSERVORE_LEVEL_CAUTION;
    }
    return OBSERVORE_LEVEL_CLEAR;
}

void observore_track_status(observore_status_t *out, int64_t now_us)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    OBSERVORE_LOCK();
    apply_decay(now_us);
    out->score = s_score;
    out->level = level_for(s_score);
    out->total_sightings = s_total_sightings;
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].classified) {
            out->device_count++;
            out->class_counts[s_devices[i].ev.cls]++;
        }
    }
    OBSERVORE_UNLOCK();
}

/* One collector for all three views.  `want`: 1 classified, 0 unclassified,
 * -1 either.  Entries are kept in sorted order as they are found, bounded by
 * `max` -- the previous shape collected all 192 slots and then insertion-sorted
 * the lot so a caller could display forty, which meant shuffling megabytes of
 * 120-byte structs on a request the console makes every two seconds. */
typedef enum { BY_NOTHING, BY_LAST_SEEN, BY_HITS } sort_key_t;

static bool precedes(const observore_event_t *a, const observore_event_t *b,
                     sort_key_t key)
{
    return (key == BY_HITS) ? a->hits > b->hits
                            : a->last_seen_us > b->last_seen_us;
}

static size_t collect(observore_event_t *out, size_t max, int want,
                      sort_key_t key)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    OBSERVORE_LOCK();
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            continue;
        }
        if (want >= 0 && s_devices[i].classified != (want == 1)) {
            continue;
        }
        const observore_event_t *ev = &s_devices[i].ev;

        if (key == BY_NOTHING) {
            if (n == max) {
                break;
            }
            out[n++] = *ev;
            continue;
        }
        /* Full and not good enough to displace the weakest kept entry. */
        if (n == max && !precedes(ev, &out[n - 1], key)) {
            continue;
        }
        size_t j = (n < max) ? n++ : max - 1;
        while (j > 0 && precedes(ev, &out[j - 1], key)) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = *ev;
    }
    OBSERVORE_UNLOCK();
    return n;
}

size_t observore_track_nearby(observore_event_t *out, size_t max)
{
    /* Busiest first: the things seen most are the ones worth naming and
     * muting, and a long tail of one-off sightings is noise. */
    return collect(out, max, 0, BY_HITS);
}

size_t observore_track_snapshot(observore_event_t *out, size_t max)
{
    return collect(out, max, 1, BY_LAST_SEEN);   /* newest first */
}

size_t observore_track_all(observore_event_t *out, size_t max)
{
    return collect(out, max, -1, BY_NOTHING);
}

size_t observore_track_drain_new(observore_event_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    OBSERVORE_LOCK();
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use && s_devices[i].classified &&
            !s_devices[i].reported) {
            s_devices[i].reported = true;
            out[n++] = s_devices[i].ev;
        }
    }
    OBSERVORE_UNLOCK();
    return n;
}

const char *observore_level_name(observore_level_t level)
{
    switch (level) {
        case OBSERVORE_LEVEL_ALERT:   return "alert";
        case OBSERVORE_LEVEL_CAUTION: return "caution";
        default:                  return "clear";
    }
}
