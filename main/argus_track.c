#include <stdio.h>
#include <string.h>

#include "argus_mute.h"
#include "argus_track.h"

#ifdef ARGUS_HOST_TEST
#define ARGUS_LOCK()   do {} while (0)
#define ARGUS_UNLOCK() do {} while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
#define ARGUS_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define ARGUS_UNLOCK() xSemaphoreGiveRecursive(s_lock)
#endif

typedef struct {
    argus_event_t ev;
    int64_t       last_scored_us;
    bool          in_use;
    bool          classified;
    bool          has_scored;   /* distinct from last_scored_us == 0, which is
                                 * a legitimate timestamp at boot */
    bool          reported;     /* already emitted to the serial event log */
} argus_slot_t;

static argus_slot_t s_devices[ARGUS_MAX_DEVICES];
static uint16_t     s_score;
static int64_t      s_last_decay_us;
static uint32_t     s_total_sightings;

void argus_track_init(void)
{
#ifndef ARGUS_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    argus_track_clear();
}

void argus_track_clear(void)
{
    ARGUS_LOCK();
    memset(s_devices, 0, sizeof(s_devices));
    s_score = 0;
    s_last_decay_us = 0;
    s_total_sightings = 0;
    ARGUS_UNLOCK();
}

static argus_slot_t *find_slot(const uint8_t mac[ARGUS_MAC_LEN])
{
    for (size_t i = 0; i < ARGUS_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            memcmp(s_devices[i].ev.mac, mac, ARGUS_MAC_LEN) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/* Claim a slot, evicting the least recently seen entry when the table is full.
 * Classified devices are never evicted in favour of an unclassified one --
 * losing a confirmed bodycam to make room for a passing phone would be the
 * wrong trade. */
static argus_slot_t *claim_slot(void)
{
    argus_slot_t *victim = NULL;
    for (size_t i = 0; i < ARGUS_MAX_DEVICES; i++) {
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
    for (size_t i = 1; i < ARGUS_MAX_DEVICES; i++) {
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
           now_us - s_last_decay_us >= ARGUS_SCORE_DECAY_INTERVAL_US) {
        s_score--;
        s_last_decay_us += ARGUS_SCORE_DECAY_INTERVAL_US;
    }
    /* Once bottomed out, stop carrying decay debt forward -- otherwise a later
     * burst would be decayed away the instant it arrived. */
    if (s_score == 0) {
        s_last_decay_us = now_us;
    }
}

/* Add points for a device, respecting the per-device cooldown so a beacon
 * shouting ten times a second cannot run the score away on its own. */
static void score_device(argus_slot_t *slot, int64_t now_us)
{
    if (slot->ev.points == 0) {
        return;
    }
    if (slot->has_scored &&
        now_us - slot->last_scored_us < ARGUS_SCORE_COOLDOWN_US) {
        return;
    }
    slot->has_scored = true;
    slot->last_scored_us = now_us;
    uint32_t next = (uint32_t)s_score + slot->ev.points;
    s_score = (next > ARGUS_SCORE_MAX) ? ARGUS_SCORE_MAX : (uint16_t)next;
}

bool argus_track_observe(const argus_observation_t *obs, int64_t now_us)
{
    if (!obs || !obs->mac) {
        return false;
    }
    if (obs->rssi != 0 && obs->rssi < ARGUS_RSSI_FLOOR) {
        return false;
    }

    argus_event_t classified;
    bool is_classified = argus_classify(obs, &classified);

    /* Suppress before the table is touched, not after.  A muted device that
     * still occupied a slot would keep being promoted by the follower
     * heuristic and keep evicting things you do care about. */
    if (argus_mute_matches(obs->mac,
                           is_classified ? classified.cls : ARGUS_CLASS_UNKNOWN,
                           obs->ssid)) {
        return false;
    }

    ARGUS_LOCK();
    apply_decay(now_us);
    s_total_sightings++;

    argus_slot_t *slot = find_slot(obs->mac);
    if (!slot) {
        slot = claim_slot();
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        memcpy(slot->ev.mac, obs->mac, ARGUS_MAC_LEN);
        slot->ev.first_seen_us = now_us;
        /* Name the vendor once, for every device -- this is what lets an
         * unrecognised MAC be read as "my own handset" and muted, and what
         * gives a follower hit something to go on. */
        slot->ev.addr_random = argus_obs_is_random(obs);
        slot->ev.vendor = slot->ev.addr_random ? NULL
                                               : argus_vendor_lookup(obs->mac);
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
    if (slot->ev.detail[0] == '\0') {
        if (obs->src == ARGUS_SRC_BLE) {
            char name[sizeof(slot->ev.detail)];
            if (argus_adv_name(obs->adv, obs->adv_len, name, sizeof(name)) &&
                name[0] != '\0') {
                snprintf(slot->ev.detail, sizeof(slot->ev.detail), "%s", name);
            }
        } else if (obs->ssid && obs->ssid[0]) {
            snprintf(slot->ev.detail, sizeof(slot->ev.detail), "%s", obs->ssid);
        }
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
    } else if (!slot->classified && obs->src == ARGUS_SRC_BLE) {
        /* The follower heuristic.  This is the piece that catches hardware
         * with no signature at all -- the reason to build the thing. */
        int64_t span = slot->ev.last_seen_us - slot->ev.first_seen_us;
        if (slot->ev.hits >= ARGUS_FOLLOWER_MIN_HITS &&
            span >= ARGUS_FOLLOWER_MIN_SPAN_US) {
            slot->ev.cls = ARGUS_CLASS_FOLLOWER;
            slot->ev.evidence = ARGUS_EVIDENCE_PERSISTENCE;
            slot->ev.points = argus_class_points(ARGUS_CLASS_FOLLOWER);
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
    ARGUS_UNLOCK();
    return reportable;
}

void argus_track_tick(int64_t now_us)
{
    ARGUS_LOCK();
    apply_decay(now_us);

    for (size_t i = 0; i < ARGUS_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            now_us - s_devices[i].ev.last_seen_us > ARGUS_DEVICE_TTL_US) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
        }
    }
    ARGUS_UNLOCK();
}

static argus_level_t level_for(uint16_t score)
{
    if (score >= 6) {
        return ARGUS_LEVEL_ALERT;
    }
    if (score >= 3) {
        return ARGUS_LEVEL_CAUTION;
    }
    return ARGUS_LEVEL_CLEAR;
}

void argus_track_status(argus_status_t *out, int64_t now_us)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    ARGUS_LOCK();
    apply_decay(now_us);
    out->score = s_score;
    out->level = level_for(s_score);
    out->total_sightings = s_total_sightings;
    for (size_t i = 0; i < ARGUS_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].classified) {
            out->device_count++;
            out->class_counts[s_devices[i].ev.cls]++;
        }
    }
    ARGUS_UNLOCK();
}

size_t argus_track_snapshot(argus_event_t *out, size_t max, int64_t now_us)
{
    (void)now_us;
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    ARGUS_LOCK();
    for (size_t i = 0; i < ARGUS_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use && s_devices[i].classified) {
            out[n++] = s_devices[i].ev;
        }
    }
    ARGUS_UNLOCK();

    /* Insertion sort, newest first.  n is bounded by the table size and this
     * runs only when the UI asks for a snapshot. */
    for (size_t i = 1; i < n; i++) {
        argus_event_t key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].last_seen_us < key.last_seen_us) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

size_t argus_track_nearby(argus_event_t *out, size_t max, int64_t now_us)
{
    (void)now_us;
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    ARGUS_LOCK();
    for (size_t i = 0; i < ARGUS_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use && !s_devices[i].classified) {
            out[n++] = s_devices[i].ev;
        }
    }
    ARGUS_UNLOCK();

    /* Busiest first: the things you see most are the things worth naming and
     * muting, and a long tail of one-off sightings is noise. */
    for (size_t i = 1; i < n; i++) {
        argus_event_t key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].hits < key.hits) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

size_t argus_track_drain_new(argus_event_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    ARGUS_LOCK();
    for (size_t i = 0; i < ARGUS_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use && s_devices[i].classified &&
            !s_devices[i].reported) {
            s_devices[i].reported = true;
            out[n++] = s_devices[i].ev;
        }
    }
    ARGUS_UNLOCK();
    return n;
}

const char *argus_level_name(argus_level_t level)
{
    switch (level) {
        case ARGUS_LEVEL_ALERT:   return "alert";
        case ARGUS_LEVEL_CAUTION: return "caution";
        default:                  return "clear";
    }
}
