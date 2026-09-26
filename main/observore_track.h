#pragma once

#include "observore_detect.h"

/* Sizing.  The table lives in internal RAM and is walked linearly on every
 * sighting, so it is kept small enough that a full scan is cheap. */
#define OBSERVORE_MAX_DEVICES 192

/* Follower heuristic: an unclassified BLE address seen at least this many
 * times, spanning at least this long, is reported as following you.  Two
 * sightings a second apart mean nothing; three sightings over five minutes
 * mean the device is going where you are going. */
#define OBSERVORE_FOLLOWER_MIN_HITS    3
#define OBSERVORE_FOLLOWER_MIN_SPAN_US (5 * 60 * 1000000LL)

/* How long a device that is trivially identifiable has to persist before
 * persistence alone means anything.
 *
 * A fixed public address and a stable broadcast name is what a washing machine
 * looks like, not what something following you looks like -- anything trying
 * not to be noticed randomises its address or stays nameless. But "identifiable"
 * is not "harmless": a cheap tracker with a fixed name from a public address
 * would still be following you, so the answer is a longer span rather than an
 * exemption. Five minutes catches a fixture in the next room; an hour does not,
 * and something that has been beside you for an hour is worth a word either
 * way. */
#define OBSERVORE_FOLLOWER_NAMED_SPAN_US (60 * 60 * 1000000LL)

/* A random address has to have come closer than this, at least once, before
 * persistence alone makes it a follower.
 *
 * A random address carries no vendor to reason with, so distance is the only
 * evidence left, and at the edge of range it is evidence of nothing. The case
 * that set this: a neighbour's gadget with a static random address sitting at
 * -82 to -90 dBm, drifting across the floor, announced afresh every time it
 * came back. Things genuinely near a person read -40 to -70. Vendor-identified
 * addresses keep the wider floor; a Ring at the edge of range is still a Ring. */
#define OBSERVORE_RANDOM_FOLLOWER_RSSI (-80)

/* How long a device stays "already announced" after it drops out of the
 * table. It is still tracked and still scored when it comes back -- the level
 * is honest -- but the digest does not repeat it. Six hours covers a device
 * fading and returning through an evening without covering one that has
 * genuinely gone and come back the next day. */
#define OBSERVORE_ANNOUNCED_TTL_US (6LL * 3600 * 1000000)
#define OBSERVORE_ANNOUNCED_MAX    32

/* Scoring. */
#define OBSERVORE_SCORE_DECAY_INTERVAL_US (60 * 1000000LL)  /* -1 point per minute */
#define OBSERVORE_SCORE_COOLDOWN_US      (120 * 1000000LL)  /* per device re-score */
#define OBSERVORE_SCORE_MAX               99

/* A device that has not been heard from in this long is evicted. */
#define OBSERVORE_DEVICE_TTL_US (30 * 60 * 1000000LL)

/* Adverts weaker than this are ignored: they are far enough away to be
 * someone else's problem, and they dominate the false-positive rate. */
#define OBSERVORE_RSSI_FLOOR (-90)

/* Following a device across an address rotation.
 *
 * A phone rotates its BLE address roughly every fifteen minutes. Keyed by
 * address alone, the tracker saw each rotation as a new device, so a
 * neighbour's handset sitting still all night produced a fresh follower alert
 * every quarter hour -- and a device that genuinely followed someone for two
 * hours was invisible, because none of its addresses lasted long enough.
 *
 * A new random address whose advert fingerprint matches a slot that went
 * quiet in the last ROTATION_WINDOW, at a similar signal strength, is taken to
 * be that slot's device. The quiet requirement is what keeps two identical
 * phones apart: the old address stops transmitting when the new one starts,
 * so a slot still being heard from a moment ago is not the one that rotated. */
#define OBSERVORE_ROTATION_WINDOW_US (20 * 60 * 1000000LL)
#define OBSERVORE_ROTATION_QUIET_US  (10 * 1000000LL)
#define OBSERVORE_ROTATION_RSSI_DB   15

typedef enum {
    OBSERVORE_LEVEL_CLEAR = 0,   /* score 0-2  */
    OBSERVORE_LEVEL_CAUTION,     /* score 3-5  */
    OBSERVORE_LEVEL_ALERT,       /* score 6+   */
} observore_level_t;

typedef struct {
    uint16_t      score;
    observore_level_t level;
    uint16_t      device_count;
    uint32_t      total_sightings;
    uint32_t      class_counts[OBSERVORE_CLASS_MAX];
} observore_status_t;

void observore_track_init(void);

/* Feed one raw observation in.  Handles classification, the follower
 * heuristic, deduplication and scoring.  Returns true when this observation
 * produced or updated a reportable device.  `now_us` is the monotonic clock
 * in microseconds. */
bool observore_track_observe(const observore_observation_t *obs, int64_t now_us);

/* Apply score decay and evict stale devices.  Call periodically. */
void observore_track_tick(int64_t now_us);

void observore_track_status(observore_status_t *out, int64_t now_us);

/* Snapshot the device table, most recently seen first.  Returns the number of
 * entries written.  Only classified devices are returned -- the unclassified
 * ones exist purely to feed the follower heuristic. */
size_t observore_track_snapshot(observore_event_t *out, size_t max);

/* Collect devices classified since the last call and mark them reported.
 * This is the serial event log: each detection is printed once, when it is
 * first identified, rather than on every repeat sighting. */
/* Snapshot the devices that were NOT classified, busiest first.  These are
 * the ones worth naming and muting before they ever trip the follower
 * heuristic -- your own phone, your own speakers, the neighbour's TV. */
size_t observore_track_nearby(observore_event_t *out, size_t max);

/* Every device currently tracked, classified or not.  Used to take a
 * baseline: mark everything in range as known. */
size_t observore_track_all(observore_event_t *out, size_t max);

/* The same walk in bounded chunks, so a caller can process every tracked
 * device without a buffer big enough to hold them all at once. Copies up to
 * `max` in-use devices from slot `*cursor` onward, advances `*cursor` past the
 * last slot examined, and returns how many were copied. Start with *cursor = 0
 * and call until it returns fewer than `max`. Lets a board with no PSRAM take a
 * full baseline from a few-KB buffer instead of a ~23 KB one. */
size_t observore_track_all_from(observore_event_t *out, size_t max, size_t *cursor);

/* Collect devices classified since the last call and mark them reported.
 * This is the serial event log: each detection is reported once, when it is
 * first identified, rather than on every repeat sighting. */
size_t observore_track_drain_new(observore_event_t *out, size_t max);

void observore_track_clear(void);

const char *observore_level_name(observore_level_t level);
