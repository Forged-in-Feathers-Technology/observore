#pragma once

#include <stddef.h>

#include "argus_types.h"

#ifdef ARGUS_HOST_TEST
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

#define ARGUS_MUTE_MAX      128
#define ARGUS_MUTE_SSID_LEN 33

typedef enum {
    ARGUS_MUTE_MAC = 0,  /* one exact address */
    ARGUS_MUTE_OUI,      /* a whole vendor prefix */
    ARGUS_MUTE_CLASS,    /* an entire class, e.g. every camera on the street */
    ARGUS_MUTE_NAME,     /* advertised name or SSID, substring, case-insensitive */
    ARGUS_MUTE_FINGERPRINT, /* stable shape of a BLE advert */
    ARGUS_MUTE_KIND_MAX
} argus_mute_kind_t;

/* Kept for the wire format and the existing API spelling; a name rule matches
 * a Wi-Fi SSID and a BLE local name alike, since both end up in the same
 * field. */
#define ARGUS_MUTE_SSID ARGUS_MUTE_NAME

typedef struct {
    uint8_t  kind;                       /* argus_mute_kind_t */
    uint8_t  mac[ARGUS_MAC_LEN];         /* MAC and OUI kinds */
    uint8_t  cls;                        /* CLASS kind */
    char     ssid[ARGUS_MUTE_SSID_LEN];  /* NAME kind */
    uint32_t fingerprint;                /* FINGERPRINT kind */
} argus_mute_rule_t;

/* Loads persisted rules.  Safe to call before NVS holds anything. */
void argus_mute_init(void);

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
bool argus_mute_matches(const uint8_t mac[ARGUS_MAC_LEN], argus_class_t cls,
                        const char *name, uint32_t fingerprint);

/* True for classes a fingerprint rule must never silence. */
bool argus_mute_class_is_protected(argus_class_t cls);

/* Adding a rule that already exists succeeds without duplicating it. */
esp_err_t argus_mute_add(const argus_mute_rule_t *rule);
esp_err_t argus_mute_remove(size_t index);
esp_err_t argus_mute_clear(void);

size_t argus_mute_count(void);
size_t argus_mute_list(argus_mute_rule_t *out, size_t max);
uint32_t argus_mute_suppressed(void);

const char *argus_mute_kind_name(argus_mute_kind_t kind);

/* Parse "AA:BB:CC:DD:EE:FF" or "AA:BB:CC" (also accepting '-' or no
 * separators).  `want` is the number of bytes required. */
bool argus_mute_parse_mac(const char *text, uint8_t *out, size_t want);

/* Map a class name as the API spells it back to the enum. */
bool argus_mute_parse_class(const char *name, argus_class_t *out);
