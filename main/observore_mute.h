#pragma once

#include <stddef.h>

#include "observore_types.h"

#ifdef OBSERVORE_HOST_TEST
/* The host test build has no ESP-IDF.  Only the handful of codes this module
 * actually returns are needed. */
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_NOT_FOUND     0x105
#else
#include "esp_err.h"
#endif

/* Suppression rules.  A detector that cries wolf at your own doorbell every
 * day is a detector you stop reading, so anything you have already judged
 * harmless can be muted -- and the rules survive a reboot, because re-muting
 * your own street every time you power on is the same problem again. */

#define OBSERVORE_MUTE_MAX      128
#define OBSERVORE_MUTE_SSID_LEN 33

typedef enum {
    OBSERVORE_MUTE_MAC = 0,  /* one exact address */
    OBSERVORE_MUTE_OUI,      /* a whole vendor prefix */
    OBSERVORE_MUTE_CLASS,    /* an entire class, e.g. every camera on the street */
    OBSERVORE_MUTE_NAME,     /* advertised name or SSID, substring, case-insensitive */
    OBSERVORE_MUTE_FINGERPRINT, /* stable shape of a BLE advert */
    OBSERVORE_MUTE_KIND_MAX
} observore_mute_kind_t;

typedef struct {
    uint8_t  kind;                       /* observore_mute_kind_t */
    uint8_t  mac[OBSERVORE_MAC_LEN];         /* MAC and OUI kinds */
    uint8_t  cls;                        /* CLASS kind */
    char     ssid[OBSERVORE_MUTE_SSID_LEN];  /* NAME kind */
    uint32_t fingerprint;                /* FINGERPRINT kind */
} observore_mute_rule_t;

/* How widely each rule has been firing, alongside the rules themselves.
 *
 * A rule that silences everything looks exactly like a quiet room: the device
 * reports "clear" and nothing says why. Two boards in one house made that
 * visible only by comparison -- one admitted eight thousand sightings while
 * the other, two metres away, admitted six. So each rule now carries what it
 * has suppressed and roughly how many different addresses it has covered, and
 * a fingerprint rule that turns out to cover a whole population of devices
 * stops being honoured. */
typedef struct {
    uint32_t suppressed;   /* sightings this rule has discarded */
    uint8_t  addresses;    /* distinct addresses seen, saturating */
    bool     disabled;     /* stopped: it silenced too much to be a device */
} observore_mute_stat_t;

/* A fingerprint rule covering more addresses than this is describing a kind of
 * device rather than one device, whatever it was written for. Phones in a
 * household share advert shapes; a rule that has matched nine different ones
 * is not muting your phone, it is muting phones. */
#define OBSERVORE_MUTE_ADDRESS_LIMIT 8

/* The shortest broadcast name a baseline will turn into a rule. Name rules
 * match as substrings -- deliberate and useful when a person types one, a trap
 * when a baseline takes whatever it hears. Anything shorter is muted by
 * address instead. */
#define OBSERVORE_BASELINE_NAME_MIN 4

/* Reads the statistics for a rule by index. False if there is no such rule. */
bool observore_mute_stat(size_t index, observore_mute_stat_t *out);

/* Loads persisted rules.  Safe to call before NVS holds anything. */
void observore_mute_init(void);

/* True when this sighting should be suppressed entirely: kept out of the
 * device table, unscored, and -- because it never reaches the table -- unable
 * to be promoted by the follower heuristic either. */
/* `name` is the device's advertised name or SSID, and `fingerprint` the
 * stable shape of its advert (0 if none).
 *
 * SAFETY: a FINGERPRINT rule identifies a kind of device rather than an
 * individual, so it is never allowed to suppress a threat class.  Muting your
 * own AirTag by fingerprint would otherwise silence a stranger's too, which is
 * precisely the thing this device exists to notice. */
bool observore_mute_matches(const uint8_t mac[OBSERVORE_MAC_LEN], observore_class_t cls,
                        const char *name, uint32_t fingerprint);

/* True for classes a fingerprint rule must never silence. */
bool observore_mute_class_is_protected(observore_class_t cls);

/* Adding a rule that already exists succeeds without duplicating it.
 * `added` (optional) reports whether the list actually grew, so a caller does
 * not have to bracket the call with observore_mute_count(). */
esp_err_t observore_mute_add(const observore_mute_rule_t *rule, bool *added);

/* Add without writing to flash; call observore_mute_save() once when done.
 *
 * Persisting inside the loop was costing a full blob rewrite and commit per
 * rule: taking a baseline of ~130 devices pushed several hundred kilobytes
 * through a 24 KB NVS partition, and the page erases stall the flash cache --
 * which stalls the BLE and Wi-Fi code running from it, blinding the detector
 * for the duration of an operation whose whole point is to be unobtrusive. */
esp_err_t observore_mute_add_deferred(const observore_mute_rule_t *rule,
                                      bool *added);
void observore_mute_save(void);
esp_err_t observore_mute_remove(size_t index);
esp_err_t observore_mute_clear(void);

size_t observore_mute_count(void);
size_t observore_mute_list(observore_mute_rule_t *out, size_t max);

/* Copy rule `index` out.  Lets a caller walk the list without standing up a
 * 6 KB copy of the whole table in internal RAM. */
bool observore_mute_get(size_t index, observore_mute_rule_t *out);
uint32_t observore_mute_suppressed(void);

const char *observore_mute_kind_name(observore_mute_kind_t kind);

/* Parse "AA:BB:CC:DD:EE:FF" or "AA:BB:CC" (also accepting '-' or no
 * separators).  `want` is the number of bytes required. */
bool observore_mute_parse_mac(const char *text, uint8_t *out, size_t want);

/* Map a class name as the API spells it back to the enum. */
bool observore_mute_parse_class(const char *name, observore_class_t *out);

/* What a baseline did, for whoever asked for it. */
typedef struct {
    size_t seen;        /* devices in the table at the time */
    size_t added;       /* new rules */
    size_t already;     /* devices a rule already covered */
    size_t by_name, by_fingerprint, by_mac;
    size_t temporary;   /* MAC rules on rotating addresses: back within the hour */
    size_t no_room;     /* rules refused because the table is full */
} observore_baseline_t;

/* Mark everything currently in range as known and start the score from a
 * clean slate. Prefers the most durable rule each device supports -- a name,
 * then the advert fingerprint where the class allows it, and only then the
 * MAC. Saves once. `scratch` must hold `cap` events; the caller owns it
 * because the size that fits depends on the board. */
void observore_mute_baseline(observore_event_t *scratch, size_t cap,
                             observore_baseline_t *out);
