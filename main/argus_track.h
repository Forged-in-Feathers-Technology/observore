#pragma once

#include "argus_detect.h"

/* Sizing.  The table lives in internal RAM and is walked linearly on every
 * sighting, so it is kept small enough that a full scan is cheap. */
#define ARGUS_MAX_DEVICES 192

/* Follower heuristic: an unclassified BLE address seen at least this many
 * times, spanning at least this long, is reported as following you.  Two
 * sightings a second apart mean nothing; three sightings over five minutes
 * mean the device is going where you are going. */
#define ARGUS_FOLLOWER_MIN_HITS    3
#define ARGUS_FOLLOWER_MIN_SPAN_US (5 * 60 * 1000000LL)

/* Scoring. */
#define ARGUS_SCORE_DECAY_INTERVAL_US (60 * 1000000LL)  /* -1 point per minute */
#define ARGUS_SCORE_COOLDOWN_US      (120 * 1000000LL)  /* per device re-score */
#define ARGUS_SCORE_MAX               99

/* A device that has not been heard from in this long is evicted. */
#define ARGUS_DEVICE_TTL_US (30 * 60 * 1000000LL)

/* Adverts weaker than this are ignored: they are far enough away to be
 * someone else's problem, and they dominate the false-positive rate. */
#define ARGUS_RSSI_FLOOR (-90)

typedef enum {
    ARGUS_LEVEL_CLEAR = 0,   /* score 0-2  */
    ARGUS_LEVEL_CAUTION,     /* score 3-5  */
    ARGUS_LEVEL_ALERT,       /* score 6+   */
} argus_level_t;

typedef struct {
    uint16_t      score;
    argus_level_t level;
    uint16_t      device_count;
    uint32_t      total_sightings;
    uint32_t      class_counts[ARGUS_CLASS_MAX];
} argus_status_t;

void argus_track_init(void);

/* Feed one raw observation in.  Handles classification, the follower
 * heuristic, deduplication and scoring.  Returns true when this observation
 * produced or updated a reportable device.  `now_us` is the monotonic clock
 * in microseconds. */
bool argus_track_observe(const argus_observation_t *obs, int64_t now_us);

/* Apply score decay and evict stale devices.  Call periodically. */
void argus_track_tick(int64_t now_us);

void argus_track_status(argus_status_t *out, int64_t now_us);

/* Snapshot the device table, most recently seen first.  Returns the number of
 * entries written.  Only classified devices are returned -- the unclassified
 * ones exist purely to feed the follower heuristic. */
size_t argus_track_snapshot(argus_event_t *out, size_t max, int64_t now_us);

/* Collect devices classified since the last call and mark them reported.
 * This is the serial event log: each detection is printed once, when it is
 * first identified, rather than on every repeat sighting. */
/* Snapshot the devices that were NOT classified, busiest first.  These are
 * the ones worth naming and muting before they ever trip the follower
 * heuristic -- your own phone, your own speakers, the neighbour's TV. */
size_t argus_track_nearby(argus_event_t *out, size_t max, int64_t now_us);

size_t argus_track_drain_new(argus_event_t *out, size_t max);

void argus_track_clear(void);

const char *argus_level_name(argus_level_t level);
