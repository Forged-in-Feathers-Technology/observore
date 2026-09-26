#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OBSERVORE_MAC_LEN 6

#define OBSERVORE_ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

/* What a sighting was classified as.  The order is meaningful: higher value =
 * more specific / more alarming, and OBSERVORE_CLASS_UNKNOWN must stay first so a
 * zeroed struct reads as "not classified". */
typedef enum {
    OBSERVORE_CLASS_UNKNOWN = 0,
    OBSERVORE_CLASS_CAMERA,           /* consumer / commercial IP camera vendor */
    OBSERVORE_CLASS_FLEET_TELEMATICS, /* dashcam + telematics fleet hardware */
    OBSERVORE_CLASS_TRACKER,          /* AirTag, SmartTag, Tile, ... */
    OBSERVORE_CLASS_SMARTGLASSES,
    OBSERVORE_CLASS_DRONE,            /* incl. OpenDroneID Remote ID broadcasts */
    OBSERVORE_CLASS_ALPR,             /* automated licence-plate readers */
    OBSERVORE_CLASS_BODYCAM,
    OBSERVORE_CLASS_FOLLOWER,         /* unidentified but persistently nearby */
    OBSERVORE_CLASS_PEER_DETECTOR,    /* another detector announcing itself */
    OBSERVORE_CLASS_MAX
} observore_class_t;

/* Which radio phase produced the sighting. */
typedef enum {
    OBSERVORE_SRC_BLE = 0,
    OBSERVORE_SRC_WIFI_SCAN,   /* active AP scan */
    OBSERVORE_SRC_WIFI_SNIFF,  /* promiscuous 802.11 capture */
    OBSERVORE_SRC_MAX
} observore_source_t;

/* How the classifier reached its conclusion -- surfaced in the UI so a hit can
 * be judged rather than just trusted. */
typedef enum {
    OBSERVORE_EVIDENCE_NONE = 0,
    OBSERVORE_EVIDENCE_OUI,
    OBSERVORE_EVIDENCE_NAME,
    OBSERVORE_EVIDENCE_SSID,
    OBSERVORE_EVIDENCE_MFG_DATA,
    OBSERVORE_EVIDENCE_SERVICE_UUID,
    OBSERVORE_EVIDENCE_PERSISTENCE,
    OBSERVORE_EVIDENCE_MAX
} observore_evidence_t;

typedef struct {
    uint8_t          oui[3];
    observore_class_t    cls;
    const char      *label;
} observore_oui_t;

/* Benign vendor prefix.  Four bytes per entry, which is what makes it
 * affordable to carry ten thousand of them: the name is an index into a
 * shared table rather than a pointer per row. */
typedef struct {
    uint8_t oui[3];
    uint8_t vendor;
} observore_vendor_oui_t;

#define OBSERVORE_LABEL_LEN 24

/* One classified sighting. */
typedef struct {
    uint8_t          mac[OBSERVORE_MAC_LEN];
    observore_class_t    cls;
    observore_source_t   src;
    observore_evidence_t evidence;
    int8_t           rssi;
    uint8_t          channel;      /* 0 when not applicable (BLE) */
    uint8_t          points;       /* score contribution */
    char             label[OBSERVORE_LABEL_LEN];
    char             detail[32];   /* BLE name or SSID, when present */
    /* Benign vendor name, or NULL.  Always points into the generated table in
     * flash, so copying an event around stays safe and free. */
    const char      *vendor;
    /* The advertiser used a randomised address, so no vendor can be known.
     * Distinct from "vendor lookup missed": one is deliberate anonymity, the
     * other is a gap in our table, and conflating them makes the UI lie. */
    bool             addr_random;
    /* Stable-advert fingerprint; 0 for Wi-Fi and for adverts with nothing
     * stable in them. */
    uint32_t         fingerprint;
    int64_t          first_seen_us;
    int64_t          last_seen_us;
    uint32_t         hits;
    /* How many times this device has changed its address while being tracked.
     * A randomised address that has rotated and is still here is the strongest
     * persistence evidence a rotating device can give: it outlasted the thing
     * designed to make it forgettable. */
    uint8_t          rotations;
} observore_event_t;

const char *observore_class_name(observore_class_t cls);
const char *observore_source_name(observore_source_t src);
const char *observore_evidence_name(observore_evidence_t ev);
