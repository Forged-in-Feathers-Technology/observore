#pragma once

#include <stddef.h>

#include "observore_notify_fmt.h"
#include "observore_types.h"

/* A single raw sighting handed to the classifier.  Fields that do not apply to
 * the source radio are left NULL / zero -- the classifier only reads what the
 * source can supply. */
typedef struct {
    const uint8_t  *mac;
    observore_source_t  src;
    int8_t          rssi;
    uint8_t         channel;

    /* Whether the transport reported the address as random.  BLE supplies
     * this; Wi-Fi has no such field and leaves it false.  Do not read it
     * directly -- use observore_obs_is_random(), which also consults the
     * locally-administered bit. */
    bool            addr_random;

    /* BLE: the raw advertising payload, still in length/type/value form. */
    const uint8_t  *adv;
    size_t          adv_len;

    /* Wi-Fi: the SSID from a beacon or scan result, NUL-terminated. */
    const char     *ssid;

    /* Wi-Fi: the frame carried an ASTM F3411 Remote ID element.  The sniffer
     * has already parsed the IEs, so it reports the fact rather than making
     * the classifier re-walk the frame. */
    bool            remote_id;
} observore_observation_t;

/* Classify one sighting.  Returns true and fills *out when the observation
 * matched a signature; returns false when it did not (the caller then decides
 * whether to hand it to the follower tracker). */
bool observore_classify(const observore_observation_t *obs, observore_event_t *out);

/* Exposed for host-side tests and for the follower tracker. */
const observore_oui_t *observore_oui_lookup(const uint8_t mac[OBSERVORE_MAC_LEN]);

/* Benign vendor name for a prefix, or NULL.  LABELLING ONLY -- this never
 * classifies a device and never contributes to the score.  Returns NULL for
 * randomised addresses, which carry no vendor information. */
const char *observore_vendor_lookup(const uint8_t mac[OBSERVORE_MAC_LEN]);
bool observore_ssid_is_suspicious(const char *ssid, char *label_out, size_t label_len);
bool observore_mac_is_random(const uint8_t mac[OBSERVORE_MAC_LEN]);

/* Case-insensitive substring search.  Exported because a NAME mute rule has to
 * match exactly the way the keyword tables match -- otherwise a user mutes a
 * string that still trips the classifier.  strcasestr() is a GNU extension and
 * is absent from the host test toolchain, hence the hand-rolled version. */
bool observore_contains_ci(const char *hay, const char *needle);

/* Whether an address carries no usable vendor information.
 *
 * Measured on real air, the two available signals disagree in BOTH
 * directions: the ESP32-S3 controller reported 9E:.., CB:.. and 27:.. as
 * BLE_ADDR_PUBLIC even though their locally-administered bit is set, and
 * reported 20:7C:3A and FD:93:05 as BLE_ADDR_RANDOM even though theirs is
 * clear.  Neither signal is trustworthy alone, so this is the union: if
 * either says random, no vendor can be claimed. */
bool observore_obs_is_random(const observore_observation_t *obs);

/* Walk a BLE advertising payload and return the first field of `type`.
 * Returns NULL when absent.  *len_out receives the value length. */
const uint8_t *observore_adv_field(const uint8_t *adv, size_t adv_len, uint8_t type,
                               size_t *len_out);

/* Copy the BLE local name (AD type 0x08 or 0x09) into buf.  Returns false when
 * the advert carries no name. */
bool observore_adv_name(const uint8_t *adv, size_t adv_len, char *buf, size_t buf_len);

/* A fingerprint of the STABLE parts of a BLE advertisement: which AD fields
 * are present and how long they are, the manufacturer's company ID, the
 * service UUIDs, and the local name.  Deliberately excludes the variable
 * payload -- a Find My advert rotates its key on every address change, and
 * hashing that would produce a fingerprint as short-lived as the MAC.
 *
 * This identifies a KIND of device, not an individual one: two identical
 * trackers produce the same fingerprint.  That is why a fingerprint mute is
 * never allowed to silence a threat class -- see observore_mute_matches().
 *
 * Returns 0 when there is nothing stable to hash. */
uint32_t observore_fingerprint(const uint8_t *adv, size_t adv_len);

/* Everything that varies per class, in one place.
 *
 * The urgency is abstract on purpose: it used to be a Gotify priority number,
 * which made one notification provider's numbering part of the threat model.
 *
 * These four attributes used to live in four switch statements across three
 * modules, plus a positional name array kept index-aligned with the enum by
 * convention alone -- so inserting a class mid-enum silently mislabelled every
 * class after it, and a new class silently scored zero and notified quietly
 * because both switches fell through to a default.  Designated initialisers
 * make the alignment explicit and a missing row obvious. */
typedef struct {
    const char *name;
    uint8_t     points;           /* contribution to the threat score */
    observore_urgency_t notify_urgency;  /* mapped per provider when sending */
    bool        protected_cls;    /* a fingerprint rule may never silence it */
} observore_class_desc_t;

const observore_class_desc_t *observore_class_desc(observore_class_t cls);

/* Points a class contributes to the threat score on each scored sighting. */
uint8_t observore_class_points(observore_class_t cls);
