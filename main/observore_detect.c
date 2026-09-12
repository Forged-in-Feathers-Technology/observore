#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "observore_detect.h"
#include "observore_oui_table.h"

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
    observore_class_t cls;
    const char   *label;
} observore_keyword_t;

/* Matched against BLE local names.  Substring, case-insensitive. */
static const observore_keyword_t BLE_NAME_KEYWORDS[] = {
    {"axon",          OBSERVORE_CLASS_BODYCAM,      "Axon bodycam"},
    {"bodycam",       OBSERVORE_CLASS_BODYCAM,      "body camera"},
    {"body-worn",     OBSERVORE_CLASS_BODYCAM,      "body camera"},
    {"body worn",     OBSERVORE_CLASS_BODYCAM,      "body camera"},
    {"bwc",           OBSERVORE_CLASS_BODYCAM,      "body camera"},
    {"motorola v",    OBSERVORE_CLASS_BODYCAM,      "Motorola VideoManager"},
    {"watchguard",    OBSERVORE_CLASS_BODYCAM,      "WatchGuard bodycam"},
    {"flock",         OBSERVORE_CLASS_ALPR,         "Flock Safety"},
    {"falcon",        OBSERVORE_CLASS_ALPR,         "Flock Falcon"},
    {"sparrow",       OBSERVORE_CLASS_ALPR,         "Flock Sparrow"},
    {"vigilant",      OBSERVORE_CLASS_ALPR,         "Vigilant ALPR"},
    {"leonardo",      OBSERVORE_CLASS_ALPR,         "Leonardo ALPR"},
    {"ray-ban",       OBSERVORE_CLASS_SMARTGLASSES, "Ray-Ban Meta"},
    {"rayban",        OBSERVORE_CLASS_SMARTGLASSES, "Ray-Ban Meta"},
    {"meta glass",    OBSERVORE_CLASS_SMARTGLASSES, "Meta glasses"},
    {"oakley meta",   OBSERVORE_CLASS_SMARTGLASSES, "Oakley Meta"},
    {"spectacles",    OBSERVORE_CLASS_SMARTGLASSES, "Snap Spectacles"},
    {"tile",          OBSERVORE_CLASS_TRACKER,      "Tile tracker"},
    {"smarttag",      OBSERVORE_CLASS_TRACKER,      "Galaxy SmartTag"},
    {"chipolo",       OBSERVORE_CLASS_TRACKER,      "Chipolo tracker"},
    {"airtag",        OBSERVORE_CLASS_TRACKER,      "AirTag"},
};

/* Matched against Wi-Fi SSIDs.  Deliberately narrower than the BLE list --
 * SSIDs are attacker-chosen free text and short needles produce noise. */
static const observore_keyword_t SSID_KEYWORDS[] = {
    {"flock",     OBSERVORE_CLASS_ALPR,   "Flock Safety"},
    {"alpr",      OBSERVORE_CLASS_ALPR,   "ALPR"},
    {"lpr-",      OBSERVORE_CLASS_ALPR,   "LPR"},
    {"axon",      OBSERVORE_CLASS_BODYCAM,"Axon"},
    {"bodycam",   OBSERVORE_CLASS_BODYCAM,"body camera"},
    {"bwc-",      OBSERVORE_CLASS_BODYCAM,"body camera"},
    {"cctv",      OBSERVORE_CLASS_CAMERA, "CCTV"},
    {"ipcam",     OBSERVORE_CLASS_CAMERA, "IP camera"},
    {"ip-cam",    OBSERVORE_CLASS_CAMERA, "IP camera"},
    {"surveil",   OBSERVORE_CLASS_CAMERA, "surveillance"},
    {"hikvision", OBSERVORE_CLASS_CAMERA, "Hikvision"},
    {"dahua",     OBSERVORE_CLASS_CAMERA, "Dahua"},
    {"verkada",   OBSERVORE_CLASS_CAMERA, "Verkada"},
    {"reolink",   OBSERVORE_CLASS_CAMERA, "Reolink"},
    {"amcrest",   OBSERVORE_CLASS_CAMERA, "Amcrest"},
    {"wyzecam",   OBSERVORE_CLASS_CAMERA, "Wyze"},
    {"ring-",     OBSERVORE_CLASS_CAMERA, "Ring"},
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

bool observore_contains_ci(const char *hay, const char *needle)
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

static void set_label(observore_event_t *ev, const char *label)
{
    snprintf(ev->label, sizeof(ev->label), "%s", label ? label : "");
}

static void set_detail(observore_event_t *ev, const char *detail)
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

bool observore_mac_is_random(const uint8_t mac[OBSERVORE_MAC_LEN])
{
    /* Bit 1 of the first octet is the locally-administered flag.  Trackers and
     * modern phones rotate randomised addresses, so an OUI lookup on these is
     * meaningless and must be skipped rather than mis-attributed. */
    return (mac[0] & 0x02) != 0;
}

const observore_oui_t *observore_oui_lookup(const uint8_t mac[OBSERVORE_MAC_LEN])
{
    if (observore_mac_is_random(mac)) {
        return NULL;
    }
    /* The generator emits the table sorted by prefix, so this is a plain
     * binary search over three bytes. */
    size_t lo = 0;
    size_t hi = OBSERVORE_OUI_TABLE_LEN;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(OBSERVORE_OUI_TABLE[mid].oui, mac, 3);
        if (cmp == 0) {
            return &OBSERVORE_OUI_TABLE[mid];
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

bool observore_obs_is_random(const observore_observation_t *obs)
{
    if (!obs || !obs->mac) {
        return true;   /* nothing known means nothing may be claimed */
    }
    return obs->addr_random || observore_mac_is_random(obs->mac);
}

const char *observore_vendor_lookup(const uint8_t mac[OBSERVORE_MAC_LEN])
{
    if (!mac || observore_mac_is_random(mac)) {
        return NULL;
    }
    size_t lo = 0, hi = OBSERVORE_VENDOR_OUIS_LEN;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(OBSERVORE_VENDOR_OUIS[mid].oui, mac, 3);
        if (cmp == 0) {
            uint8_t v = OBSERVORE_VENDOR_OUIS[mid].vendor;
            return (v < OBSERVORE_VENDOR_NAMES_LEN) ? OBSERVORE_VENDOR_NAMES[v] : NULL;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const uint8_t *observore_adv_field(const uint8_t *adv, size_t adv_len, uint8_t type,
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

bool observore_adv_name(const uint8_t *adv, size_t adv_len, char *buf, size_t buf_len)
{
    size_t len = 0;
    const uint8_t *p = observore_adv_field(adv, adv_len, AD_TYPE_NAME_COMPLETE, &len);
    if (!p) {
        p = observore_adv_field(adv, adv_len, AD_TYPE_NAME_SHORT, &len);
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

/* First matching row, or NULL.  One scanner for both keyword tables: the SSID
 * path used to find a row, return only its label, and then walk the table a
 * second time to recover the class -- two scans that could pick different rows
 * if either ever gained a precondition. */
static const observore_keyword_t *match_keyword(const observore_keyword_t *tbl,
                                                size_t n, const char *text)
{
    if (!text || !*text) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (observore_contains_ci(text, tbl[i].needle)) {
            return &tbl[i];
        }
    }
    return NULL;
}

bool observore_ssid_is_suspicious(const char *ssid, char *label_out, size_t label_len)
{
    const observore_keyword_t *hit =
        match_keyword(SSID_KEYWORDS, OBSERVORE_ARRLEN(SSID_KEYWORDS), ssid);
    if (!hit) {
        return false;
    }
    if (label_out && label_len) {
        snprintf(label_out, label_len, "%s", hit->label);
    }
    return true;
}

/* FNV-1a, 32-bit. */
static void fnv(uint32_t *h, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        *h ^= data[i];
        *h *= 16777619u;
    }
}

static void fnv_byte(uint32_t *h, uint8_t b)
{
    fnv(h, &b, 1);
}

uint32_t observore_fingerprint(const uint8_t *adv, size_t adv_len)
{
    if (!adv || adv_len == 0) {
        return 0;
    }

    uint32_t h = 2166136261u;
    bool any = false;
    size_t i = 0;

    while (i < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0 || i + 1 + field_len > adv_len) {
            break;
        }
        uint8_t type = adv[i + 1];
        const uint8_t *val = &adv[i + 2];
        size_t val_len = field_len - 1;

        /* Structure alone is a signal: which fields, in what order, how big. */
        fnv_byte(&h, type);
        fnv_byte(&h, (uint8_t)val_len);
        any = true;

        switch (type) {
            case AD_TYPE_FLAGS:
                fnv(&h, val, val_len < 1 ? val_len : 1);
                break;
            case AD_TYPE_UUID16_PARTIAL:
            case AD_TYPE_UUID16_COMPLETE:
                fnv(&h, val, val_len);          /* UUIDs are stable */
                break;
            case AD_TYPE_SERVICE_DATA_16:
                fnv(&h, val, val_len < 2 ? val_len : 2);  /* the UUID only */
                break;
            case AD_TYPE_MFG_DATA:
                /* Company ID only.  Everything after it is where the rotating
                 * key or counter lives. */
                fnv(&h, val, val_len < 2 ? val_len : 2);
                break;
            case AD_TYPE_NAME_SHORT:
            case AD_TYPE_NAME_COMPLETE:
                fnv(&h, val, val_len);
                break;
            default:
                break;                           /* type and length only */
        }
        i += 1 + field_len;
    }

    if (!any) {
        return 0;
    }
    /* Never return 0 for a real fingerprint -- 0 means "none". */
    return h ? h : 1u;
}

/* Urgency is abstract and translated per provider in observore_notify_fmt.c.
 *
 * "Protected" means a fingerprint mute rule may never silence the class: a
 * fingerprint identifies a KIND of device, so muting your own tracker that way
 * would silence a stranger's too. */
static const observore_class_desc_t CLASS_DESC[OBSERVORE_CLASS_MAX] = {
    [OBSERVORE_CLASS_UNKNOWN]          = {"unknown",          0, OBSERVORE_URGENCY_LOW,    false},
    [OBSERVORE_CLASS_CAMERA]           = {"camera",           1, OBSERVORE_URGENCY_LOW,    false},
    [OBSERVORE_CLASS_FLEET_TELEMATICS] = {"fleet-telematics", 2, OBSERVORE_URGENCY_LOW,    false},
    [OBSERVORE_CLASS_TRACKER]          = {"tracker",          3, OBSERVORE_URGENCY_HIGH,   true },
    [OBSERVORE_CLASS_SMARTGLASSES]     = {"smart-glasses",    3, OBSERVORE_URGENCY_NORMAL, true },
    [OBSERVORE_CLASS_DRONE]            = {"drone",            3, OBSERVORE_URGENCY_NORMAL, true },
    [OBSERVORE_CLASS_ALPR]             = {"alpr",             5, OBSERVORE_URGENCY_URGENT, true },
    [OBSERVORE_CLASS_BODYCAM]          = {"bodycam",          5, OBSERVORE_URGENCY_URGENT, true },
    [OBSERVORE_CLASS_FOLLOWER]         = {"follower",         4, OBSERVORE_URGENCY_HIGH,   true },
};

const observore_class_desc_t *observore_class_desc(observore_class_t cls)
{
    return (cls > OBSERVORE_CLASS_UNKNOWN && cls < OBSERVORE_CLASS_MAX)
               ? &CLASS_DESC[cls]
               : &CLASS_DESC[OBSERVORE_CLASS_UNKNOWN];
}

uint8_t observore_class_points(observore_class_t cls)
{
    return observore_class_desc(cls)->points;
}

/* ------------------------------------------------------------------ */
/* BLE signature matching                                             */
/* ------------------------------------------------------------------ */

/* Returns true when the advert lists `uuid` in either 16-bit UUID list. */
static bool adv_has_uuid16(const uint8_t *adv, size_t adv_len, uint16_t uuid)
{
    const uint8_t types[] = {AD_TYPE_UUID16_COMPLETE, AD_TYPE_UUID16_PARTIAL};
    for (size_t t = 0; t < OBSERVORE_ARRLEN(types); t++) {
        size_t len = 0;
        const uint8_t *p = observore_adv_field(adv, adv_len, types[t], &len);
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

static bool match_ble_signature(const observore_observation_t *obs, observore_event_t *ev)
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
        ev->cls = OBSERVORE_CLASS_DRONE;
        ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
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
    const uint8_t *mfg = observore_adv_field(adv, adv_len, AD_TYPE_MFG_DATA, &mfg_len);
    if (mfg && mfg_len >= 4) {
        uint16_t company = le16(mfg);
        const uint8_t *payload = mfg + 2;
        size_t payload_len = mfg_len - 2;

        if (company == COMPANY_APPLE && payload_len >= 2 &&
            payload[0] == APPLE_TYPE_FINDMY && payload[1] == APPLE_FINDMY_LEN) {
            ev->cls = OBSERVORE_CLASS_TRACKER;
            ev->evidence = OBSERVORE_EVIDENCE_MFG_DATA;
            set_label(ev, "Find My tracker");
            return true;
        }
        /* Note there is deliberately no bare COMPANY_SAMSUNG rule here.
         * Every Samsung phone, watch and earbud advertises 0x0075, so matching
         * the company ID alone reports a crowded room as four trackers.  The
         * SmartTag is identified by its 0xFD5A service data below instead. */
        if (company == COMPANY_META || company == COMPANY_META_TECH) {
            ev->cls = OBSERVORE_CLASS_SMARTGLASSES;
            ev->evidence = OBSERVORE_EVIDENCE_MFG_DATA;
            set_label(ev, "Meta wearable");
            return true;
        }
    }

    /* --- Tracker service UUIDs --------------------------------------- */
    if (adv_has_uuid16(adv, adv_len, UUID16_TILE)) {
        ev->cls = OBSERVORE_CLASS_TRACKER;
        ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Tile tracker");
        return true;
    }
    if (adv_has_uuid16(adv, adv_len, UUID16_SAMSUNG_FIND) ||
        adv_service_data(adv, adv_len, UUID16_SAMSUNG_FIND, &sd_len)) {
        ev->cls = OBSERVORE_CLASS_TRACKER;
        ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Galaxy SmartTag");
        return true;
    }
    if (adv_service_data(adv, adv_len, UUID16_GOOGLE_FAST_PAIR, &sd_len)) {
        /* Fast Pair also carries Google's Find Hub tags.  Lower confidence
         * than the Apple case -- ordinary headphones advertise this too --
         * so it is reported as a tracker but scored like one hit, not
         * escalated on its own. */
        ev->cls = OBSERVORE_CLASS_TRACKER;
        ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Fast Pair / Find Hub");
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

bool observore_classify(const observore_observation_t *obs, observore_event_t *out)
{
    if (!obs || !obs->mac || !out) {
        return false;
    }

    observore_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.mac, obs->mac, OBSERVORE_MAC_LEN);
    ev.src = obs->src;
    ev.rssi = obs->rssi;
    ev.channel = obs->channel;

    bool matched = false;

    /* Vendor prefix first: it is the strongest signal available and applies to
     * every source.  BLE reports the address type on the wire, which is
     * authoritative; the locally-administered bit is only a fallback for
     * Wi-Fi, where no such field exists. */
    const observore_oui_t *oui = observore_obs_is_random(obs) ? NULL
                                                     : observore_oui_lookup(obs->mac);
    if (oui) {
        ev.cls = oui->cls;
        ev.evidence = OBSERVORE_EVIDENCE_OUI;
        set_label(&ev, oui->label);
        matched = true;
    }

    /* An ASTM Remote ID element is unambiguous and outranks everything else,
     * including the vendor prefix -- plenty of drones fly on a generic Wi-Fi
     * module whose OUI says nothing. */
    if (obs->remote_id) {
        ev.cls = OBSERVORE_CLASS_DRONE;
        ev.evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
        set_label(&ev, "Remote ID drone");
        if (obs->ssid && *obs->ssid) {
            set_detail(&ev, obs->ssid);
        }
        ev.points = observore_class_points(ev.cls);
        *out = ev;
        return true;
    }

    if (obs->src == OBSERVORE_SRC_BLE) {
        char name[OBSERVORE_LABEL_LEN];
        bool have_name = observore_adv_name(obs->adv, obs->adv_len, name, sizeof(name));
        if (have_name) {
            set_detail(&ev, name);
        }

        /* A payload signature beats a vendor prefix: trackers rotate their
         * MACs, so the advert contents are the only reliable evidence. */
        observore_event_t sig = ev;
        if (match_ble_signature(obs, &sig)) {
            ev = sig;
            matched = true;
        } else if (have_name) {
            const observore_keyword_t *hit = match_keyword(
                BLE_NAME_KEYWORDS, OBSERVORE_ARRLEN(BLE_NAME_KEYWORDS), name);
            if (hit) {
                ev.cls = hit->cls;
                ev.evidence = OBSERVORE_EVIDENCE_NAME;
                set_label(&ev, hit->label);
                matched = true;
            }
        }
    } else if (obs->ssid && *obs->ssid) {
        set_detail(&ev, obs->ssid);
        const observore_keyword_t *hit =
            match_keyword(SSID_KEYWORDS, OBSERVORE_ARRLEN(SSID_KEYWORDS), obs->ssid);
        if (hit) {
            /* Only let an SSID keyword override the OUI when the OUI said
             * nothing -- a named vendor is the better answer.  Note this
             * deliberately differs from the BLE name path above, which does
             * override the OUI: an SSID is free text anyone can choose, while a
             * BLE local name accompanies a payload we have already inspected. */
            if (!oui) {
                ev.cls = hit->cls;
                ev.evidence = OBSERVORE_EVIDENCE_SSID;
                set_label(&ev, hit->label);
            }
            matched = true;
        }
    }

    if (!matched) {
        return false;
    }

    ev.points = observore_class_points(ev.cls);
    *out = ev;
    return true;
}

/* ------------------------------------------------------------------ */
/* Enum names                                                         */
/* ------------------------------------------------------------------ */

const char *observore_class_name(observore_class_t cls)
{
    return observore_class_desc(cls)->name;
}

const char *observore_source_name(observore_source_t src)
{
    static const char *names[OBSERVORE_SRC_MAX] = {"ble", "wifi-scan", "wifi-sniff"};
    return (src < OBSERVORE_SRC_MAX) ? names[src] : "?";
}

const char *observore_evidence_name(observore_evidence_t ev)
{
    static const char *names[OBSERVORE_EVIDENCE_MAX] = {
        "none", "oui", "ble-name", "ssid", "mfg-data", "service-uuid",
        "persistence",
    };
    return (ev < OBSERVORE_EVIDENCE_MAX) ? names[ev] : "?";
}
