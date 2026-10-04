#include <stdio.h>
#include <string.h>

#include "observore_mute.h"
#include "observore_monitors.h"
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
    bool          in_use;
    bool          classified;
    /* Whether this device has already been handed to the notifier, so a
     * digest names it once rather than every cycle it remains in range. */
    bool          reported;
    /* Which journey the device was on when this one became a follower. */
    uint32_t      first_journey;
    /* Evidence gathered while the board was away from home.
     *
     * `away_rssi` is the strongest this device was heard while travelling,
     * against `home_rssi`, the strongest it had managed before setting off.
     * Something carried along holds its signal; something left behind either
     * goes silent or arrives twenty decibels down. */
    bool          heard_away;
    int8_t        home_rssi;
    int8_t        away_rssi;
    uint16_t      away_hits;
} observore_slot_t;

static observore_slot_t s_devices[OBSERVORE_MAX_DEVICES];
static uint32_t     s_total_sightings;

/* How many journeys the board has made, pushed in from the motion sensor.
 * Zero for ever on the seven boards that cannot feel one. */
static uint32_t     s_journeys;

/* How much fainter a device may be while away and still be said to have
 * come along. A pocket moves a few decibels; a house left behind drops
 * twenty or more, and most of it stops being heard at all. The same number
 * as the access points use, for the same reason and with the same meaning. */
#define OBSERVORE_TAILING_FADE_DB 12

static bool s_at_far_end;

void observore_track_set_at_far_end(bool at_far_end)
{
    if (at_far_end == s_at_far_end) {
        return;
    }
    OBSERVORE_LOCK();
    s_at_far_end = at_far_end;

    if (at_far_end) {
        /* Arrived somewhere. Remember how loudly each device has ever
         * managed to be heard -- which, for anything that lives in the
         * house, is its strength at the house -- and start listening for
         * which of them is here too. */
        for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
            if (!s_devices[i].in_use) {
                continue;
            }
            s_devices[i].home_rssi  = s_devices[i].ev.rssi;
            s_devices[i].away_rssi  = -128;
            s_devices[i].away_hits  = 0;
            s_devices[i].heard_away = false;
        }
        OBSERVORE_UNLOCK();
        return;
    }

    /* The far end is over, so the evidence is complete. Whatever was beside
     * the board while it was there came with it. */
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        observore_slot_t *slot = &s_devices[i];
        if (!slot->in_use || !slot->classified ||
            slot->ev.cls != OBSERVORE_CLASS_FOLLOWER) {
            continue;
        }
        if (!slot->heard_away) {
            continue;            /* not there at all */
        }
        if (slot->away_hits < OBSERVORE_TAILING_MIN_HITS) {
            continue;            /* heard in passing, not throughout */
        }
        if (slot->away_rssi < OBSERVORE_TAILING_NEAR_RSSI) {
            continue;            /* audible, but never close to anybody */
        }
        if (slot->home_rssi - slot->away_rssi >= OBSERVORE_TAILING_FADE_DB) {
            continue;            /* there, but far fainter: left behind */
        }
        slot->ev.cls      = OBSERVORE_CLASS_TAILING;
        slot->ev.evidence = OBSERVORE_EVIDENCE_BEHAVIOUR;
        slot->ev.points   = observore_class_points(OBSERVORE_CLASS_TAILING);
        snprintf(slot->ev.label, sizeof(slot->ev.label), "came with you");
        /* Announced again, as what it has become: it was reported as a
         * follower, which is what it was then. */
        slot->reported = false;
    }
    OBSERVORE_UNLOCK();
}

void observore_track_set_journeys(uint32_t journeys)
{
    s_journeys = journeys;
}

/* Devices announced recently, so that one which fades and returns is not
 * announced again. Keyed by address for a static one and by advert fingerprint
 * for a rotating one, since that is what survives the rotation. */
typedef struct {
    uint8_t  mac[OBSERVORE_MAC_LEN];
    uint32_t fingerprint;
    int64_t  at_us;
} announced_t;
static announced_t s_announced[OBSERVORE_ANNOUNCED_MAX];
static size_t      s_announced_next;


void observore_track_init(void)
{
    memset(s_announced, 0, sizeof(s_announced));
    s_announced_next = 0;
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
    s_total_sightings = 0;
    OBSERVORE_UNLOCK();
}

static bool announced_recently(const observore_event_t *ev, int64_t now_us)
{
    for (size_t i = 0; i < OBSERVORE_ANNOUNCED_MAX; i++) {
        const announced_t *a = &s_announced[i];
        if (a->at_us == 0 || now_us - a->at_us > OBSERVORE_ANNOUNCED_TTL_US) {
            continue;
        }
        if (memcmp(a->mac, ev->mac, OBSERVORE_MAC_LEN) == 0) {
            return true;
        }
        if (ev->addr_random && ev->fingerprint != 0 &&
            a->fingerprint == ev->fingerprint) {
            return true;
        }
    }
    return false;
}

static void note_announced(const observore_event_t *ev, int64_t now_us)
{
    announced_t *a = &s_announced[s_announced_next++ % OBSERVORE_ANNOUNCED_MAX];
    memcpy(a->mac, ev->mac, OBSERVORE_MAC_LEN);
    a->fingerprint = ev->addr_random ? ev->fingerprint : 0;
    a->at_us = now_us;
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

/* The slot a freshly rotated address most likely belongs to, or NULL.
 *
 * Only for random BLE addresses with something stable to match on. Among the
 * candidates -- same fingerprint, random, quiet for at least ROTATION_QUIET but
 * heard within ROTATION_WINDOW, within ROTATION_RSSI_DB of this sighting, and
 * not carrying a different name -- the one heard from most recently wins,
 * because it is the one whose old address fell silent last.
 *
 * A fingerprint identifies a kind of device rather than one device, so this
 * can be wrong in a room with two identical handsets. The quiet requirement
 * makes that rare, and the cost of being wrong is one merged pair rather than
 * a missed threat: the merged device is still reported, still persistent, and
 * still where it was. */
static observore_slot_t *find_rotated_slot(const observore_observation_t *obs,
                                           uint32_t fingerprint,
                                           const char *name, int64_t now_us)
{
    if (obs->src != OBSERVORE_SRC_BLE || fingerprint == 0 ||
        !observore_obs_is_random(obs)) {
        return NULL;
    }
    observore_slot_t *best = NULL;
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        observore_slot_t *c = &s_devices[i];
        if (!c->in_use || c->ev.src != OBSERVORE_SRC_BLE || !c->ev.addr_random ||
            c->ev.fingerprint != fingerprint ||
            memcmp(c->ev.mac, obs->mac, OBSERVORE_MAC_LEN) == 0) {
            continue;
        }
        int64_t silent = now_us - c->ev.last_seen_us;
        if (silent < OBSERVORE_ROTATION_QUIET_US || silent > OBSERVORE_ROTATION_WINDOW_US) {
            continue;
        }
        if (obs->rssi != 0 && c->ev.rssi != 0) {
            int diff = (int)obs->rssi - (int)c->ev.rssi;
            if (diff > OBSERVORE_ROTATION_RSSI_DB || diff < -OBSERVORE_ROTATION_RSSI_DB) {
                continue;
            }
        }
        if (name && name[0] && c->ev.detail[0] && strcmp(name, c->ev.detail) != 0) {
            continue;       /* both named, and differently: not the same device */
        }
        if (!best || c->ev.last_seen_us > best->ev.last_seen_us) {
            best = c;
        }
    }
    return best;
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
    s_total_sightings++;

    observore_slot_t *slot = find_slot(obs->mac);
    if (!slot) {
        slot = find_rotated_slot(obs, fingerprint, name, now_us);
        if (slot) {
            /* Same device, new address. Everything it has earned -- hits, first
             * sighting, classification, score -- carries over, and it is not
             * announced again. Only the key changes. */
            memcpy(slot->ev.mac, obs->mac, OBSERVORE_MAC_LEN);
            if (slot->ev.rotations < 255) {
                slot->ev.rotations++;
            }
        }
    }
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
        /* ev.rssi is the strongest seen, so this asks whether the device was
         * ever within the floor, not whether it is right now. */
        bool close_enough = !slot->ev.addr_random ||
                            slot->ev.rssi >= OBSERVORE_RANDOM_FOLLOWER_RSSI;
        /* Something that publishes both a fixed address and a name has opted
         * out of being hard to identify, so persistence on its own says much
         * less. It is held to a longer span rather than excluded: see the
         * constant for why an exemption would be the wrong shape. */
        bool trivially_identifiable = !slot->ev.addr_random &&
                                      slot->ev.detail[0] != '\0';
        int64_t needed = trivially_identifiable
                             ? OBSERVORE_FOLLOWER_NAMED_SPAN_US
                             : OBSERVORE_FOLLOWER_MIN_SPAN_US;
        if (slot->ev.hits >= OBSERVORE_FOLLOWER_MIN_HITS &&
            span >= needed && close_enough) {
            slot->ev.cls = OBSERVORE_CLASS_FOLLOWER;
            slot->first_journey = s_journeys;
            slot->ev.evidence = OBSERVORE_EVIDENCE_PERSISTENCE;
            slot->ev.points = observore_class_points(OBSERVORE_CLASS_FOLLOWER);
            snprintf(slot->ev.label, sizeof(slot->ev.label), "persistent %s",
                     slot->ev.vendor        ? slot->ev.vendor
                     : slot->ev.addr_random ? "(random MAC)"
                                            : "device");
            slot->classified = true;
        }
    }

    /* A rotating address that is still here after changing has outlasted the
     * one thing meant to make it forgettable. Say so on the label, because it
     * is the strongest persistence evidence a random address can offer and the
     * thing a reader most needs to know about it. Fits OBSERVORE_LABEL_LEN
     * at three digits, which the counter cannot exceed. */
    if (slot->classified && slot->ev.cls == OBSERVORE_CLASS_FOLLOWER &&
        slot->ev.addr_random && slot->ev.rotations > 0) {
        snprintf(slot->ev.label, sizeof(slot->ev.label), "rotated %ux, persists",
                 (unsigned)slot->ev.rotations);
    }

    /* Evidence for the journey, gathered as it happens: the strongest this
     * device manages while the board is away from where it set off. */
    if (s_at_far_end) {
        if (obs->rssi > slot->away_rssi) {
            slot->away_rssi = (int8_t)obs->rssi;
        }
        if (slot->away_hits < UINT16_MAX) {
            slot->away_hits++;
        }
        slot->heard_away = true;
    }

    bool reportable = slot->classified;
    if (reportable) {
        /* Still scored -- the level must be honest -- but a follower that was
         * announced in the last few hours and merely faded and returned is not
         * news, so it is marked as already reported before the digest sees it.
         *
         * Followers only. A follower is an inference from persistence, and a
         * persistent thing coming back is the same inference again. A body
         * camera, an ALPR unit, a drone or a tracker is a signature match, and
         * one of those coming back is exactly what the device exists to say. */
        if (!slot->reported && slot->ev.cls == OBSERVORE_CLASS_FOLLOWER &&
            announced_recently(&slot->ev, now_us)) {
            slot->reported = true;
        }
    }
    OBSERVORE_UNLOCK();
    return reportable;
}

void observore_track_tick(int64_t now_us)
{
    OBSERVORE_LOCK();

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
    if (score >= OBSERVORE_SCORE_ALERT) {
        return OBSERVORE_LEVEL_ALERT;
    }
    if (score >= OBSERVORE_SCORE_CAUTION) {
        return OBSERVORE_LEVEL_CAUTION;
    }
    return OBSERVORE_LEVEL_CLEAR;
}

/* The score is what is in front of the device, not a history of it.
 *
 * It used to accumulate: every device added its points again every two
 * minutes, against a decay of one point a minute in total. Anything worth two
 * points or more therefore outran the decay on its own, so the score climbed
 * to its ceiling and stayed -- ninety-nine meant "something persistent has
 * been here a while", never "how much is here". A number permanently at
 * maximum is not read, and a level permanently at alert is not believed.
 *
 * So it is a sum over what is currently tracked, computed when asked. It rises
 * when something arrives and falls when it leaves; the table's own thirty
 * minute expiry is what makes it fall, and is slow enough that nothing
 * flickers. Nothing to decay, no per-device cooldown, no accumulator to pin.
 *
 * Sustained presence is not lost, it has simply moved to where it belongs: a
 * device does not become a follower until it has been there five minutes, and
 * surviving an address rotation is what distinguishes it afterwards. Duration
 * decides what something IS; the score says what is here. */
void observore_track_status(observore_status_t *out, int64_t now_us)
{
    if (!out) {
        return;
    }
    (void)now_us;
    memset(out, 0, sizeof(*out));
    OBSERVORE_LOCK();
    out->total_sightings = s_total_sightings;

    uint32_t score = 0;
    uint16_t presence = 0;
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use || !s_devices[i].classified) {
            continue;
        }
        const observore_event_t *e = &s_devices[i].ev;
        out->device_count++;
        out->class_counts[e->cls]++;

        /* A monitor that has been switched off contributes no score.
         *
         * The sighting is still tracked and still counted above -- "off" means
         * keep seeing and stop reporting, so switching it back on shows what
         * has been around all along rather than starting a blank history. The
         * count of what is off travels with the verdict so that "clear" is
         * never mistaken for "looked and found nothing". */
        if (!observore_monitors_enabled(e->cls)) {
            continue;
        }

        uint16_t points = e->points;
        if (e->cls == OBSERVORE_CLASS_FOLLOWER) {
            /* The whole class is capped, rotation included. A follower is
             * unidentified by definition, and the device should not raise an
             * alarm about something it cannot name. See the cap for what two
             * earlier attempts got wrong about this. */
            if (e->rotations == 0) {
                points = OBSERVORE_FOLLOWER_PRESENT_POINTS;
            }
            if (presence >= OBSERVORE_FOLLOWER_SCORE_CAP) {
                continue;
            }
            if (presence + points > OBSERVORE_FOLLOWER_SCORE_CAP) {
                points = (uint16_t)(OBSERVORE_FOLLOWER_SCORE_CAP - presence);
            }
            presence = (uint16_t)(presence + points);
        }
        score += points;
    }
    out->score = (score > OBSERVORE_SCORE_MAX) ? OBSERVORE_SCORE_MAX
                                               : (uint16_t)score;
    out->level = level_for(out->score);
    out->monitors_off = observore_monitors_off_count();
    OBSERVORE_UNLOCK();
}

/* One collector for all three views.  `want`: 1 classified, 0 unclassified,
 * -1 either.  Entries are kept in sorted order as they are found, bounded by
 * `max` -- the previous shape collected all 192 slots and then insertion-sorted
 * the lot so a caller could display forty, which meant shuffling megabytes of
 * 120-byte structs on a request the console makes every two seconds. */
typedef enum { BY_NOTHING, BY_LAST_SEEN, BY_HITS, BY_WEIGHT } sort_key_t;

/* What a device is worth for the purpose of ordering a list.
 *
 * Its class points, except for a follower -- which carries four in the table
 * and can never contribute more than the class cap to a score. Sorting by the
 * raw four put followers above trackers and above a Flipper, so a trip that
 * found nine devices showed a screenful of unidentified phones with the
 * identified equipment beneath them. Exactly the failure the weight ordering
 * was added to prevent, one layer further in. */
static uint16_t list_weight(const observore_event_t *e)
{
    if (e->cls == OBSERVORE_CLASS_FOLLOWER &&
        e->points > OBSERVORE_FOLLOWER_SCORE_CAP) {
        return OBSERVORE_FOLLOWER_SCORE_CAP;
    }
    return e->points;
}

static bool precedes(const observore_event_t *a, const observore_event_t *b,
                     sort_key_t key)
{
    /* Weight first, recency as the tiebreak. The screen shows twelve rows of
     * a table that holds far more, so what falls off the bottom matters: with
     * recency alone, a crowd hides a finding. Ten promotions in one evening
     * pushed everything else off a 466-pixel screen, and a body camera would
     * have gone with them. */
    if (key == BY_WEIGHT) {
        uint16_t wa = list_weight(a), wb = list_weight(b);
        if (wa != wb) {
            return wa > wb;
        }
    }
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
        /* Findings only: a switched-off monitor does not appear in them. The
         * unclassified view is deliberately untouched -- "unknown" is the
         * absence of a monitor rather than one of them, and hiding things the
         * device could not name is the opposite of what this is for. */
        if (s_devices[i].classified &&
            !observore_monitors_enabled(s_devices[i].ev.cls)) {
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
    return collect(out, max, 1, BY_WEIGHT);   /* heaviest first, then newest */
}

size_t observore_track_forget_muted(void)
{
    size_t gone = 0;
    OBSERVORE_LOCK();
    for (size_t i = 0; i < OBSERVORE_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            continue;
        }
        const observore_event_t *e = &s_devices[i].ev;
        /* The non-counting matcher: a sweep must not charge these rows to
         * whichever rule covered them. */
        if (observore_mute_would_match(e->mac, e->cls,
                                       e->detail[0] ? e->detail : NULL,
                                       e->fingerprint)) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            gone++;
        }
    }
    OBSERVORE_UNLOCK();
    return gone;
}

size_t observore_track_all(observore_event_t *out, size_t max)
{
    return collect(out, max, -1, BY_NOTHING);
}

size_t observore_track_all_from(observore_event_t *out, size_t max, size_t *cursor)
{
    if (!out || max == 0 || !cursor) {
        return 0;
    }
    size_t n = 0;
    OBSERVORE_LOCK();
    size_t i = *cursor;
    for (; i < OBSERVORE_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use) {
            out[n++] = s_devices[i].ev;
        }
    }
    *cursor = i;
    OBSERVORE_UNLOCK();
    return n;
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
            note_announced(&s_devices[i].ev, s_devices[i].ev.last_seen_us);
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
