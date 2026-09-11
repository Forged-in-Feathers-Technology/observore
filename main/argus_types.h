#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARGUS_MAC_LEN 6

/* What a sighting was classified as.  The order is meaningful: higher value =
 * more specific / more alarming, and ARGUS_CLASS_UNKNOWN must stay first so a
 * zeroed struct reads as "not classified". */
typedef enum {
    ARGUS_CLASS_UNKNOWN = 0,
    ARGUS_CLASS_CAMERA,           /* consumer / commercial IP camera vendor */
    ARGUS_CLASS_FLEET_TELEMATICS, /* dashcam + telematics fleet hardware */
    ARGUS_CLASS_TRACKER,          /* AirTag, SmartTag, Tile, ... */
    ARGUS_CLASS_SMARTGLASSES,
    ARGUS_CLASS_DRONE,            /* incl. OpenDroneID Remote ID broadcasts */
    ARGUS_CLASS_ALPR,             /* automated licence-plate readers */
    ARGUS_CLASS_BODYCAM,
    ARGUS_CLASS_FOLLOWER,         /* unidentified but persistently nearby */
    ARGUS_CLASS_MAX
} argus_class_t;

/* Which radio phase produced the sighting. */
typedef enum {
    ARGUS_SRC_BLE = 0,
    ARGUS_SRC_WIFI_SCAN,   /* active AP scan */
    ARGUS_SRC_WIFI_SNIFF,  /* promiscuous 802.11 capture */
    ARGUS_SRC_MAX
} argus_source_t;

/* How the classifier reached its conclusion -- surfaced in the UI so a hit can
 * be judged rather than just trusted. */
typedef enum {
    ARGUS_EVIDENCE_NONE = 0,
    ARGUS_EVIDENCE_OUI,
    ARGUS_EVIDENCE_NAME,
    ARGUS_EVIDENCE_SSID,
    ARGUS_EVIDENCE_MFG_DATA,
    ARGUS_EVIDENCE_SERVICE_UUID,
    ARGUS_EVIDENCE_PERSISTENCE,
    ARGUS_EVIDENCE_MAX
} argus_evidence_t;

typedef struct {
    uint8_t          oui[3];
    argus_class_t    cls;
    const char      *label;
} argus_oui_t;

#define ARGUS_LABEL_LEN 24

/* One classified sighting. */
typedef struct {
    uint8_t          mac[ARGUS_MAC_LEN];
    argus_class_t    cls;
    argus_source_t   src;
    argus_evidence_t evidence;
    int8_t           rssi;
    uint8_t          channel;      /* 0 when not applicable (BLE) */
    uint8_t          points;       /* score contribution */
    char             label[ARGUS_LABEL_LEN];
    char             detail[32];   /* BLE name or SSID, when present */
    int64_t          first_seen_us;
    int64_t          last_seen_us;
    uint32_t         hits;
} argus_event_t;

const char *argus_class_name(argus_class_t cls);
const char *argus_source_name(argus_source_t src);
const char *argus_evidence_name(argus_evidence_t ev);
