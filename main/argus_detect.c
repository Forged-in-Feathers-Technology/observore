#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "argus_detect.h"
#include "argus_oui_table.h"

/* ------------------------------------------------------------------ */
/* BLE assigned numbers                                               */
/* ------------------------------------------------------------------ */

#define AD_TYPE_FLAGS              0x01
#define AD_TYPE_UUID16_PARTIAL     0x02
#define AD_TYPE_UUID16_COMPLETE    0x03
#define AD_TYPE_NAME_SHORT         0x08
#define AD_TYPE_NAME_COMPLETE      0x09
#define AD_TYPE_SERVICE_DATA_16    0x16
#define AD_TYPE_MFG_DATA           0xFF

/* Bluetooth SIG company identifiers (little-endian on the wire). */
#define COMPANY_APPLE              0x004C
#define COMPANY_SAMSUNG            0x0075
#define COMPANY_META               0x01AB
#define COMPANY_META_TECH          0x058E

/* 16-bit service UUIDs. */
#define UUID16_ASTM_REMOTE_ID      0xFFFA  /* ASTM F3411 / OpenDroneID */
#define UUID16_TILE                0xFEED
#define UUID16_SAMSUNG_FIND        0xFD5A  /* Galaxy SmartTag offline finding */
#define UUID16_GOOGLE_FAST_PAIR    0xFE2C  /* Fast Pair, incl. Find Hub tags */

/* ASTM address-space application code carried in the 0xFFFA service data. */
#define ASTM_APP_CODE_ODID         0x0D

/* Apple manufacturer-data payload types. */
#define APPLE_TYPE_FINDMY          0x12    /* Find My network broadcast */
#define APPLE_FINDMY_LEN           0x19

/* ------------------------------------------------------------------ */
/* Name / SSID keyword signatures                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char   *needle;   /* lowercase substring */
    argus_class_t cls;
    const char   *label;
} argus_keyword_t;

/* Matched against BLE local names.  Substring, case-insensitive. */
static const argus_keyword_t BLE_NAME_KEYWORDS[] = {
    {"axon",          ARGUS_CLASS_BODYCAM,      "Axon bodycam"},
    {"bodycam",       ARGUS_CLASS_BODYCAM,      "body camera"},
    {"body-worn",     ARGUS_CLASS_BODYCAM,      "body camera"},
    {"body worn",     ARGUS_CLASS_BODYCAM,      "body camera"},
    {"bwc",           ARGUS_CLASS_BODYCAM,      "body camera"},
    {"motorola v",    ARGUS_CLASS_BODYCAM,      "Motorola VideoManager"},
    {"watchguard",    ARGUS_CLASS_BODYCAM,      "WatchGuard bodycam"},
    {"flock",         ARGUS_CLASS_ALPR,         "Flock Safety"},
    {"falcon",        ARGUS_CLASS_ALPR,         "Flock Falcon"},
    {"sparrow",       ARGUS_CLASS_ALPR,         "Flock Sparrow"},
    {"vigilant",      ARGUS_CLASS_ALPR,         "Vigilant ALPR"},
    {"leonardo",      ARGUS_CLASS_ALPR,         "Leonardo ALPR"},
    {"ray-ban",       ARGUS_CLASS_SMARTGLASSES, "Ray-Ban Meta"},
    {"rayban",        ARGUS_CLASS_SMARTGLASSES, "Ray-Ban Meta"},
    {"meta glass",    ARGUS_CLASS_SMARTGLASSES, "Meta glasses"},
    {"oakley meta",   ARGUS_CLASS_SMARTGLASSES, "Oakley Meta"},
    {"spectacles",    ARGUS_CLASS_SMARTGLASSES, "Snap Spectacles"},
    {"tile",          ARGUS_CLASS_TRACKER,      "Tile tracker"},
    {"smarttag",      ARGUS_CLASS_TRACKER,      "Galaxy SmartTag"},
    {"chipolo",       ARGUS_CLASS_TRACKER,      "Chipolo tracker"},
    {"airtag",        ARGUS_CLASS_TRACKER,      "AirTag"},
};

/* Matched against Wi-Fi SSIDs.  Deliberately narrower than the BLE list --
 * SSIDs are attacker-chosen free text and short needles produce noise. */
static const argus_keyword_t SSID_KEYWORDS[] = {
    {"flock",     ARGUS_CLASS_ALPR,   "Flock Safety"},
    {"alpr",      ARGUS_CLASS_ALPR,   "ALPR"},
    {"lpr-",      ARGUS_CLASS_ALPR,   "LPR"},
    {"axon",      ARGUS_CLASS_BODYCAM,"Axon"},
    {"bodycam",   ARGUS_CLASS_BODYCAM,"body camera"},
    {"bwc-",      ARGUS_CLASS_BODYCAM,"body camera"},
    {"cctv",      ARGUS_CLASS_CAMERA, "CCTV"},
    {"ipcam",     ARGUS_CLASS_CAMERA, "IP camera"},
    {"ip-cam",    ARGUS_CLASS_CAMERA, "IP camera"},
    {"surveil",   ARGUS_CLASS_CAMERA, "surveillance"},
    {"hikvision", ARGUS_CLASS_CAMERA, "Hikvision"},
    {"dahua",     ARGUS_CLASS_CAMERA, "Dahua"},
    {"verkada",   ARGUS_CLASS_CAMERA, "Verkada"},
    {"reolink",   ARGUS_CLASS_CAMERA, "Reolink"},
    {"amcrest",   ARGUS_CLASS_CAMERA, "Amcrest"},
    {"wyzecam",   ARGUS_CLASS_CAMERA, "Wyze"},
    {"ring-",     ARGUS_CLASS_CAMERA, "Ring"},
};

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive substring search.  strcasestr() is a GNU extension and is
 * not available in every toolchain this file is compiled by (the host test
 * build included), so it is spelled out here. */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) {
        return false;
    }
    for (const char *h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && lower(*a) == lower(*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

static void set_label(argus_event_t *ev, const char *label)
{
    snprintf(ev->label, sizeof(ev->label), "%s", label ? label : "");
}

static void set_detail(argus_event_t *ev, const char *detail)
{
    snprintf(ev->detail, sizeof(ev->detail), "%s", detail ? detail : "");
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* Public helpers                                                     */
/* ------------------------------------------------------------------ */

bool argus_mac_is_random(const uint8_t mac[ARGUS_MAC_LEN])
{
    /* Bit 1 of the first octet is the locally-administered flag.  Trackers and
     * modern phones rotate randomised addresses, so an OUI lookup on these is
     * meaningless and must be skipped rather than mis-attributed. */
    return (mac[0] & 0x02) != 0;
}

const argus_oui_t *argus_oui_lookup(const uint8_t mac[ARGUS_MAC_LEN])
{
    if (argus_mac_is_random(mac)) {
        return NULL;
    }
    /* The generator emits the table sorted by prefix, so this is a plain
     * binary search over three bytes. */
    size_t lo = 0;
    size_t hi = ARGUS_OUI_TABLE_LEN;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(ARGUS_OUI_TABLE[mid].oui, mac, 3);
        if (cmp == 0) {
            return &ARGUS_OUI_TABLE[mid];
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const uint8_t *argus_adv_field(const uint8_t *adv, size_t adv_len, uint8_t type,
                               size_t *len_out)
{
    size_t i = 0;
    while (i < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0 || i + 1 + field_len > adv_len) {
            break;  /* padding or a malformed advert -- stop, do not guess */
        }
        if (adv[i + 1] == type) {
            if (len_out) {
                *len_out = field_len - 1;
            }
            return &adv[i + 2];
        }
        i += 1 + field_len;
    }
    return NULL;
}

bool argus_adv_name(const uint8_t *adv, size_t adv_len, char *buf, size_t buf_len)
{
    size_t len = 0;
    const uint8_t *p = argus_adv_field(adv, adv_len, AD_TYPE_NAME_COMPLETE, &len);
    if (!p) {
        p = argus_adv_field(adv, adv_len, AD_TYPE_NAME_SHORT, &len);
    }
    if (!p || len == 0 || buf_len == 0) {
        return false;
    }
    size_t n = len < buf_len - 1 ? len : buf_len - 1;
    for (size_t i = 0; i < n; i++) {
        /* Advertised names are remote-controlled bytes.  Anything outside
         * printable ASCII becomes '.' so it cannot corrupt the log or the
         * web UI downstream. */
        buf[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
    }
    buf[n] = '\0';
    return true;
}

bool argus_ssid_is_suspicious(const char *ssid, char *label_out, size_t label_len)
{
    if (!ssid || !*ssid) {
        return false;
    }
    for (size_t i = 0; i < ARRLEN(SSID_KEYWORDS); i++) {
        if (contains_ci(ssid, SSID_KEYWORDS[i].needle)) {
            if (label_out && label_len) {
                snprintf(label_out, label_len, "%s", SSID_KEYWORDS[i].label);
            }
            return true;
        }
    }
    return false;
}

uint8_t argus_class_points(argus_class_t cls)
{
    switch (cls) {
        case ARGUS_CLASS_BODYCAM:          return 5;
        case ARGUS_CLASS_ALPR:             return 5;
        case ARGUS_CLASS_FOLLOWER:         return 4;
        case ARGUS_CLASS_TRACKER:          return 3;
        case ARGUS_CLASS_DRONE:            return 3;
        case ARGUS_CLASS_SMARTGLASSES:     return 3;
        case ARGUS_CLASS_FLEET_TELEMATICS: return 2;
        case ARGUS_CLASS_CAMERA:           return 1;
        default:                           return 0;
    }
}

/* ------------------------------------------------------------------ */
/* BLE signature matching                                             */
/* ------------------------------------------------------------------ */

/* Returns true when the advert lists `uuid` in either 16-bit UUID list. */
static bool adv_has_uuid16(const uint8_t *adv, size_t adv_len, uint16_t uuid)
{
    const uint8_t types[] = {AD_TYPE_UUID16_COMPLETE, AD_TYPE_UUID16_PARTIAL};
    for (size_t t = 0; t < ARRLEN(types); t++) {
        size_t len = 0;
        const uint8_t *p = argus_adv_field(adv, adv_len, types[t], &len);
        if (!p) {
            continue;
        }
        for (size_t i = 0; i + 1 < len; i += 2) {
            if (le16(&p[i]) == uuid) {
                return true;
            }
        }
    }
    return false;
}

/* Service data (AD 0x16) begins with the 16-bit UUID it belongs to. */
static const uint8_t *adv_service_data(const uint8_t *adv, size_t adv_len,
                                       uint16_t uuid, size_t *len_out)
{
    size_t i = 0;
    while (i < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0 || i + 1 + field_len > adv_len) {
            break;
        }
        if (adv[i + 1] == AD_TYPE_SERVICE_DATA_16 && field_len >= 3 &&
            le16(&adv[i + 2]) == uuid) {
            *len_out = field_len - 3;
            return &adv[i + 4];
        }
        i += 1 + field_len;
    }
    return NULL;
}

static bool match_ble_signature(const argus_observation_t *obs, argus_event_t *ev)
{
    const uint8_t *adv = obs->adv;
    size_t adv_len = obs->adv_len;
    if (!adv || adv_len == 0) {
        return false;
    }

    /* --- OpenDroneID / ASTM F3411 Remote ID --------------------------
     * The service data under 0xFFFA opens with the ASTM application code;
     * anything else under that UUID is a different ASTM application. */
    size_t sd_len = 0;
    const uint8_t *sd = adv_service_data(adv, adv_len, UUID16_ASTM_REMOTE_ID, &sd_len);
    if (sd && sd_len >= 1 && sd[0] == ASTM_APP_CODE_ODID) {
        ev->cls = ARGUS_CLASS_DRONE;
        ev->evidence = ARGUS_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Remote ID drone");
        return true;
    }

    /* --- Apple Find My -----------------------------------------------
     * Type 0x12 with payload length 0x19 is the separated-mode broadcast an
     * AirTag (or any Find My accessory away from its owner) emits.  This is
     * the case that matters: a tag travelling with you but not with its
     * owner.  Paired-and-present Apple gear advertises other payload types
     * and is deliberately not flagged. */
    size_t mfg_len = 0;
    const uint8_t *mfg = argus_adv_field(adv, adv_len, AD_TYPE_MFG_DATA, &mfg_len);
    if (mfg && mfg_len >= 4) {
        uint16_t company = le16(mfg);
        const uint8_t *payload = mfg + 2;
        size_t payload_len = mfg_len - 2;

        if (company == COMPANY_APPLE && payload_len >= 2 &&
            payload[0] == APPLE_TYPE_FINDMY && payload[1] == APPLE_FINDMY_LEN) {
            ev->cls = ARGUS_CLASS_TRACKER;
            ev->evidence = ARGUS_EVIDENCE_MFG_DATA;
            set_label(ev, "Find My tracker");
            return true;
        }
        if (company == COMPANY_SAMSUNG) {
            ev->cls = ARGUS_CLASS_TRACKER;
            ev->evidence = ARGUS_EVIDENCE_MFG_DATA;
            set_label(ev, "Samsung tracker");
            return true;
        }
        if (company == COMPANY_META || company == COMPANY_META_TECH) {
            ev->cls = ARGUS_CLASS_SMARTGLASSES;
            ev->evidence = ARGUS_EVIDENCE_MFG_DATA;
            set_label(ev, "Meta wearable");
            return true;
        }
    }

    /* --- Tracker service UUIDs --------------------------------------- */
    if (adv_has_uuid16(adv, adv_len, UUID16_TILE)) {
        ev->cls = ARGUS_CLASS_TRACKER;
        ev->evidence = ARGUS_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Tile tracker");
        return true;
    }
    if (adv_has_uuid16(adv, adv_len, UUID16_SAMSUNG_FIND) ||
        adv_service_data(adv, adv_len, UUID16_SAMSUNG_FIND, &sd_len)) {
        ev->cls = ARGUS_CLASS_TRACKER;
        ev->evidence = ARGUS_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Galaxy SmartTag");
        return true;
    }
    if (adv_service_data(adv, adv_len, UUID16_GOOGLE_FAST_PAIR, &sd_len)) {
        /* Fast Pair also carries Google's Find Hub tags.  Lower confidence
         * than the Apple case -- ordinary headphones advertise this too --
         * so it is reported as a tracker but scored like one hit, not
         * escalated on its own. */
        ev->cls = ARGUS_CLASS_TRACKER;
        ev->evidence = ARGUS_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Fast Pair / Find Hub");
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

bool argus_classify(const argus_observation_t *obs, argus_event_t *out)
{
    if (!obs || !obs->mac || !out) {
        return false;
    }

    argus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.mac, obs->mac, ARGUS_MAC_LEN);
    ev.src = obs->src;
    ev.rssi = obs->rssi;
    ev.channel = obs->channel;

    bool matched = false;

    /* Vendor prefix first: it is the strongest signal available and applies to
     * every source.  Randomised MACs are skipped inside the lookup. */
    const argus_oui_t *oui = argus_oui_lookup(obs->mac);
    if (oui) {
        ev.cls = oui->cls;
        ev.evidence = ARGUS_EVIDENCE_OUI;
        set_label(&ev, oui->label);
        matched = true;
    }

    /* An ASTM Remote ID element is unambiguous and outranks everything else,
     * including the vendor prefix -- plenty of drones fly on a generic Wi-Fi
     * module whose OUI says nothing. */
    if (obs->remote_id) {
        ev.cls = ARGUS_CLASS_DRONE;
        ev.evidence = ARGUS_EVIDENCE_SERVICE_UUID;
        set_label(&ev, "Remote ID drone");
        if (obs->ssid && *obs->ssid) {
            set_detail(&ev, obs->ssid);
        }
        ev.points = argus_class_points(ev.cls);
        *out = ev;
        return true;
    }

    if (obs->src == ARGUS_SRC_BLE) {
        char name[ARGUS_LABEL_LEN];
        bool have_name = argus_adv_name(obs->adv, obs->adv_len, name, sizeof(name));
        if (have_name) {
            set_detail(&ev, name);
        }

        /* A payload signature beats a vendor prefix: trackers rotate their
         * MACs, so the advert contents are the only reliable evidence. */
        argus_event_t sig = ev;
        if (match_ble_signature(obs, &sig)) {
            ev = sig;
            matched = true;
        } else if (have_name) {
            for (size_t i = 0; i < ARRLEN(BLE_NAME_KEYWORDS); i++) {
                if (contains_ci(name, BLE_NAME_KEYWORDS[i].needle)) {
                    ev.cls = BLE_NAME_KEYWORDS[i].cls;
                    ev.evidence = ARGUS_EVIDENCE_NAME;
                    set_label(&ev, BLE_NAME_KEYWORDS[i].label);
                    matched = true;
                    break;
                }
            }
        }
    } else if (obs->ssid && *obs->ssid) {
        set_detail(&ev, obs->ssid);
        char label[ARGUS_LABEL_LEN];
        if (argus_ssid_is_suspicious(obs->ssid, label, sizeof(label))) {
            /* Only let an SSID keyword override the OUI when the OUI said
             * nothing -- a named vendor is the better answer. */
            if (!oui) {
                for (size_t i = 0; i < ARRLEN(SSID_KEYWORDS); i++) {
                    if (contains_ci(obs->ssid, SSID_KEYWORDS[i].needle)) {
                        ev.cls = SSID_KEYWORDS[i].cls;
                        break;
                    }
                }
                ev.evidence = ARGUS_EVIDENCE_SSID;
                set_label(&ev, label);
            }
            matched = true;
        }
    }

    if (!matched) {
        return false;
    }

    ev.points = argus_class_points(ev.cls);
    *out = ev;
    return true;
}

/* ------------------------------------------------------------------ */
/* Enum names                                                         */
/* ------------------------------------------------------------------ */

const char *argus_class_name(argus_class_t cls)
{
    static const char *names[ARGUS_CLASS_MAX] = {
        "unknown", "camera", "fleet-telematics", "tracker",
        "smart-glasses", "drone", "alpr", "bodycam", "follower",
    };
    return (cls < ARGUS_CLASS_MAX) ? names[cls] : "unknown";
}

const char *argus_source_name(argus_source_t src)
{
    static const char *names[ARGUS_SRC_MAX] = {"ble", "wifi-scan", "wifi-sniff"};
    return (src < ARGUS_SRC_MAX) ? names[src] : "?";
}

const char *argus_evidence_name(argus_evidence_t ev)
{
    static const char *names[ARGUS_EVIDENCE_MAX] = {
        "none", "oui", "ble-name", "ssid", "mfg-data", "service-uuid",
        "persistence",
    };
    return (ev < ARGUS_EVIDENCE_MAX) ? names[ev] : "?";
}
