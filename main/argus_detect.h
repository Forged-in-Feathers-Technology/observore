#pragma once

#include <stddef.h>

#include "argus_types.h"

/* A single raw sighting handed to the classifier.  Fields that do not apply to
 * the source radio are left NULL / zero -- the classifier only reads what the
 * source can supply. */
typedef struct {
    const uint8_t  *mac;
    argus_source_t  src;
    int8_t          rssi;
    uint8_t         channel;

    /* BLE: the raw advertising payload, still in length/type/value form. */
    const uint8_t  *adv;
    size_t          adv_len;

    /* Wi-Fi: the SSID from a beacon or scan result, NUL-terminated. */
    const char     *ssid;

    /* Wi-Fi: the frame carried an ASTM F3411 Remote ID element.  The sniffer
     * has already parsed the IEs, so it reports the fact rather than making
     * the classifier re-walk the frame. */
    bool            remote_id;
} argus_observation_t;

/* Classify one sighting.  Returns true and fills *out when the observation
 * matched a signature; returns false when it did not (the caller then decides
 * whether to hand it to the follower tracker). */
bool argus_classify(const argus_observation_t *obs, argus_event_t *out);

/* Exposed for host-side tests and for the follower tracker. */
const argus_oui_t *argus_oui_lookup(const uint8_t mac[ARGUS_MAC_LEN]);
bool argus_ssid_is_suspicious(const char *ssid, char *label_out, size_t label_len);
bool argus_mac_is_random(const uint8_t mac[ARGUS_MAC_LEN]);

/* Walk a BLE advertising payload and return the first field of `type`.
 * Returns NULL when absent.  *len_out receives the value length. */
const uint8_t *argus_adv_field(const uint8_t *adv, size_t adv_len, uint8_t type,
                               size_t *len_out);

/* Copy the BLE local name (AD type 0x08 or 0x09) into buf.  Returns false when
 * the advert carries no name. */
bool argus_adv_name(const uint8_t *adv, size_t adv_len, char *buf, size_t buf_len);

/* Points a class contributes to the threat score on each scored sighting. */
uint8_t argus_class_points(argus_class_t cls);
