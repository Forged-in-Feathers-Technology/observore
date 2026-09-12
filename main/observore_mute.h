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

/* Kept for the wire format and the existing API spelling; a name rule matches
 * a Wi-Fi SSID and a BLE local name alike, since both end up in the same
 * field. */
#define OBSERVORE_MUTE_SSID OBSERVORE_MUTE_NAME

typedef struct {
    uint8_t  kind;                       /* observore_mute_kind_t */
    uint8_t  mac[OBSERVORE_MAC_LEN];         /* MAC and OUI kinds */
    uint8_t  cls;                        /* CLASS kind */
    char     ssid[OBSERVORE_MUTE_SSID_LEN];  /* NAME kind */
    uint32_t fingerprint;                /* FINGERPRINT kind */
} observore_mute_rule_t;

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

/* Adding a rule that already exists succeeds without duplicating it. */
esp_err_t observore_mute_add(const observore_mute_rule_t *rule);
esp_err_t observore_mute_remove(size_t index);
esp_err_t observore_mute_clear(void);

size_t observore_mute_count(void);
size_t observore_mute_list(observore_mute_rule_t *out, size_t max);
uint32_t observore_mute_suppressed(void);

const char *observore_mute_kind_name(observore_mute_kind_t kind);

/* Parse "AA:BB:CC:DD:EE:FF" or "AA:BB:CC" (also accepting '-' or no
 * separators).  `want` is the number of bytes required. */
bool observore_mute_parse_mac(const char *text, uint8_t *out, size_t want);

/* Map a class name as the API spells it back to the enum. */
bool observore_mute_parse_class(const char *name, observore_class_t *out);
