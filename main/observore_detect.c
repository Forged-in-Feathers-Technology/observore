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

/* The SIG's reserved "not a real product" company ID, shared by every hobby
 * project that declined to squat a registered one. Meaningless on its own. */
#define COMPANY_NONPRODUCTION      0xFFFF
/* Flipper Devices Inc. in the SIG's company list. Worth stating because the
 * wrong value is widespread: ESP32 Marauder comments Flipper's company ID as
 * 0x0FBA and every project that copied the constant inherited it. 0x0FBA is
 * Cosonic Intelligent Technologies, who make headsets -- flagging their
 * customers as hacking tools. Flipper is 0x0E29. */
#define COMPANY_FLIPPER            0x0E29
/* The three 16-bit service UUIDs a Flipper advertises, one per hardware
 * colour. */
#define UUID16_FLIPPER_A           0x3081
#define UUID16_FLIPPER_B           0x3082
#define UUID16_FLIPPER_C           0x3083

/* SquachMesh: 'S' 'Q' 'M' '1', then a version byte. */
static const uint8_t SQUACHMESH_MAGIC[4] = {0x53, 0x51, 0x4D, 0x31};
#define SQUACHMESH_VERSION         1
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
    /* A Flipper advertises "Flipper " and then whatever the owner named it. */
    {"flipper",       OBSERVORE_CLASS_HUNTER,       "Flipper Zero"},
    /* Enphase solar equipment: the Envoy gateway and the Encharge batteries
     * both broadcast their model and serial, sit still forever, and were
     * arriving as followers. */
    {"envoy",         OBSERVORE_CLASS_FIXTURE,      "Enphase Envoy"},
    {"encharg",       OBSERVORE_CLASS_FIXTURE,      "Enphase Encharge"},
    {"enphase",       OBSERVORE_CLASS_FIXTURE,      "Enphase solar"},
};

/* Matched against Wi-Fi SSIDs.  Deliberately narrower than the BLE list --
 * SSIDs are attacker-chosen free text and short needles produce noise. */
static const observore_keyword_t SSID_KEYWORDS[] = {
    /* Hak5 hold no IEEE registration, so the management AP's default name is
     * the only thing to go on. The underscore is load-bearing: "pineapple"
     * alone would claim somebody's home network. */
    {"pineapple_", OBSERVORE_CLASS_HUNTER, "WiFi Pineapple"},
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
    /* Seen on hardware: a UniFi camera in factory setup mode broadcasts
     * "Setup UVC G3 Micro (6AEB)", which matched nothing here and was reported
     * only by the follower heuristic.  UVC is UniFi Video Camera. */
    {"uvc",       OBSERVORE_CLASS_CAMERA, "UniFi camera"},
    {"foscam",    OBSERVORE_CLASS_CAMERA, "Foscam"},
    {"lorex",     OBSERVORE_CLASS_CAMERA, "Lorex"},
    {"swann",     OBSERVORE_CLASS_CAMERA, "Swann"},

    /* Drones announce themselves on their control access point.  Remote ID
     * catches them over BLE, but only while they are transmitting it; the
     * control link is up from the moment the aircraft is powered on.
     *
     * Also seen on hardware: "HolyStoneGIM-b79437D", alongside a Remote ID
     * beacon the BLE path did catch. */
    {"holystone", OBSERVORE_CLASS_DRONE,  "HolyStone"},
    {"dji-",      OBSERVORE_CLASS_DRONE,  "DJI"},
    {"mavic",     OBSERVORE_CLASS_DRONE,  "DJI Mavic"},
    {"tello-",    OBSERVORE_CLASS_DRONE,  "DJI Tello"},
    {"autel",     OBSERVORE_CLASS_DRONE,  "Autel"},
    {"skydio",    OBSERVORE_CLASS_DRONE,  "Skydio"},
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
    /* Worth zero points on purpose. Another detector in the room is a fact
     * about the room, not a threat in it, and a class that raised the score
     * would make a meetup read as an incident. It is still announced, because
     * "somebody else is watching too" is the sort of thing a person wants to
     * know, and it is unprotected because muting the kind wholesale is a
     * reasonable thing to want. */
    [OBSERVORE_CLASS_PEER_DETECTOR]    = {"peer-detector",    0, OBSERVORE_URGENCY_LOW,    false},
    /* An attack in progress rather than a device in the room, and scored to
     * match: knocking devices off a network is how an attacker forces a
     * handshake to capture, or simply takes a camera offline. Protected,
     * because nothing about a fingerprint should ever be able to silence
     * this. */
    [OBSERVORE_CLASS_DEAUTH]           = {"deauth-flood",     5, OBSERVORE_URGENCY_URGENT, true },
    /* Everything else here is equipment that watches. This is equipment that
     * transmits at other radios -- a Flipper, a pwnagotchi, a Pineapple. Three
     * points rather than five: the presence of the tool is capability, where a
     * flood in progress is an act. Protected, for the same reason a tracker
     * is: muting the kind would silence a stranger's as well as your own. */
    [OBSERVORE_CLASS_HUNTER]           = {"hunter",           3, OBSERVORE_URGENCY_NORMAL, true },
    /* Equipment bolted to a building, named and scored at nothing. Worth
     * recognising rather than ignoring: a solar gateway that says "Envoy" was
     * being promoted to follower -- "unidentified but persistently nearby" --
     * when it is neither unidentified nor going anywhere. Naming it is more
     * honest than muting it, and it survives a Clear ignores. */
    [OBSERVORE_CLASS_FIXTURE]          = {"fixture",          0, OBSERVORE_URGENCY_LOW,    false},
    /* Reported, barely scored. Every pair of budget earbuds in pairing mode
     * used to arrive as a tracker worth three points, which is how a crowded
     * cafe reads as four trackers and how a person learns to stop believing
     * the score. One point keeps it visible without letting it escalate. */
    [OBSERVORE_CLASS_ACCESSORY]        = {"accessory",        1, OBSERVORE_URGENCY_LOW,    false},
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
        /* --- SquachMesh ----------------------------------------------
         * SquachWatch is another open-source detector on the same board
         * family, and in mesh mode it announces itself so two of them can
         * draw each other on screen. The payload rides under company ID
         * 0xFFFF -- the SIG's reserved non-production ID -- so the company
         * alone means nothing: it is shared with every other hobby project
         * that made the same honest choice. The four magic bytes are what
         * make this specific, which is exactly why their own header says the
         * magic is not optional.
         *
         * Read from their source rather than guessed from the air: four bytes
         * "SQM1", a version, a little-endian appearance word, a reserved
         * flags byte -- eight bytes, or twenty when the owner typed a name
         * that follows NUL-padded. */
        if (company == COMPANY_NONPRODUCTION && payload_len >= 8 &&
            memcmp(payload, SQUACHMESH_MAGIC, sizeof(SQUACHMESH_MAGIC)) == 0) {
            ev->cls = OBSERVORE_CLASS_PEER_DETECTOR;
            ev->evidence = OBSERVORE_EVIDENCE_MFG_DATA;
            /* Their version byte is theirs to bump. An unknown one is still
             * a SquachWatch -- refusing to name it would be pretending not
             * to have recognised something we plainly did. */
            uint8_t version = payload[4];
            /* A typed name is twelve bytes at offset 8, and it is a stranger's
             * bytes landing in a string this device renders. Copied one
             * printable character at a time and stopped at anything else,
             * which is the same rule their own decoder applies for the same
             * reason. */
            char name[13] = {0};
            size_t n = 0;
            if (payload_len >= 20) {
                for (size_t k = 0; k < 12; k++) {
                    uint8_t c = payload[8 + k];
                    if (c == 0) {
                        break;
                    }
                    if (c < 0x20 || c > 0x7E) {
                        n = 0;          /* not a name we will repeat */
                        break;
                    }
                    name[n++] = (char)c;
                }
            }
            name[n] = '\0';
            /* What it is goes in the label; what its owner called it goes in
             * the detail, which is also the field a baseline turns into a
             * name rule. A name that was not wholly printable is dropped
             * rather than sanitised: half of somebody's chosen name is not
             * their name, and nothing here needs to display it. */
            set_label(ev, version == SQUACHMESH_VERSION ? "SquachWatch"
                                                        : "SquachWatch (newer)");
            if (n > 0) {
                set_detail(ev, name);
            }
            return true;
        }
        /* Note there is deliberately no bare COMPANY_SAMSUNG rule here.
         * Every Samsung phone, watch and earbud advertises 0x0075, so matching
         * the company ID alone reports a crowded room as four trackers.  The
         * SmartTag is identified by its 0xFD5A service data below instead. */
        if (company == COMPANY_FLIPPER) {
            ev->cls = OBSERVORE_CLASS_HUNTER;
            ev->evidence = OBSERVORE_EVIDENCE_MFG_DATA;
            set_label(ev, "Flipper Zero");
            return true;
        }
        if (company == COMPANY_META || company == COMPANY_META_TECH) {
            ev->cls = OBSERVORE_CLASS_SMARTGLASSES;
            ev->evidence = OBSERVORE_EVIDENCE_MFG_DATA;
            set_label(ev, "Meta wearable");
            return true;
        }
    }

    if (adv_has_uuid16(adv, adv_len, UUID16_FLIPPER_A) ||
        adv_has_uuid16(adv, adv_len, UUID16_FLIPPER_B) ||
        adv_has_uuid16(adv, adv_len, UUID16_FLIPPER_C)) {
        ev->cls = OBSERVORE_CLASS_HUNTER;
        ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
        set_label(ev, "Flipper Zero");
        return true;
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
        /* Fast Pair carries both Google's Find Hub tags and every pair of
         * earbuds in the world announcing itself to pair, and the two are not
         * equally interesting.
         *
         * Exactly three bytes of service data is the discoverable frame: a
         * 24-bit model ID and nothing else, which is what a device in pairing
         * mode sends. That is the one form worth separating, and it is
         * separated by SCORE rather than by silence -- an accessory is still
         * reported, still listed, still visible. Anything else under this UUID
         * stays a tracker at full weight.
         *
         * Deliberately asymmetric. Mistaking a tracker for an accessory costs
         * two points on a device that is still on the screen; mistaking it for
         * nothing would cost the detection, and there is no byte in this
         * advert that reliably tells a tag from a headphone. A tracker in
         * pairing mode is one its owner is setting up, not one following
         * somebody. */
        if (sd_len == 3) {
            ev->cls = OBSERVORE_CLASS_ACCESSORY;
            ev->evidence = OBSERVORE_EVIDENCE_SERVICE_UUID;
            set_label(ev, "Fast Pair pairing");
            return true;
        }
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
    /* A flood outranks everything, including a known vendor: whose equipment
     * it is matters less than the fact that it is happening. */
    if (obs->deauth_flood) {
        ev.cls = OBSERVORE_CLASS_DEAUTH;
        ev.evidence = OBSERVORE_EVIDENCE_BEHAVIOUR;
        set_label(&ev, "deauth flood");
        if (obs->ssid && *obs->ssid) {
            set_detail(&ev, obs->ssid);
        }
        ev.points = observore_class_points(ev.cls);
        *out = ev;
        return true;
    }

    if (obs->pwnagotchi) {
        ev.cls = OBSERVORE_CLASS_HUNTER;
        ev.evidence = OBSERVORE_EVIDENCE_BEHAVIOUR;
        set_label(&ev, "pwnagotchi");
        if (obs->ssid && *obs->ssid) {
            set_detail(&ev, obs->ssid);
        }
        ev.points = observore_class_points(ev.cls);
        *out = ev;
        return true;
    }

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
    } else if ((obs->ssid && *obs->ssid) || !observore_wps_empty(obs->wps)) {
        /* What the access point says about itself, when it says anything.
         *
         * An OUI is an inference from an address block. A WPS element is the
         * device stating its own manufacturer and model in plain text, which
         * is better evidence and survives the case this project is weakest at:
         * a camera whose vendor prefix is unregistered or simply missing from
         * the table. So the WPS strings are matched first, and the detail line
         * prefers them over the SSID, which is free text somebody chose. */
        const observore_keyword_t *wps_hit = NULL;
        if (!observore_wps_empty(obs->wps)) {
            const char *fields[] = {obs->wps->manufacturer, obs->wps->model,
                                    obs->wps->device_name};
            for (size_t i = 0; i < OBSERVORE_ARRLEN(fields) && !wps_hit; i++) {
                wps_hit = match_keyword(SSID_KEYWORDS,
                                        OBSERVORE_ARRLEN(SSID_KEYWORDS), fields[i]);
            }
            char shown[OBSERVORE_WPS_FIELD_LEN * 2];
            if (obs->wps->manufacturer[0] && obs->wps->model[0]) {
                snprintf(shown, sizeof(shown), "%s %s",
                         obs->wps->manufacturer, obs->wps->model);
            } else {
                snprintf(shown, sizeof(shown), "%s",
                         obs->wps->manufacturer[0] ? obs->wps->manufacturer
                                                   : (obs->wps->model[0]
                                                      ? obs->wps->model
                                                      : obs->wps->device_name));
            }
            set_detail(&ev, shown);
        } else {
            set_detail(&ev, obs->ssid);
        }
        if (wps_hit) {
            /* Unlike an SSID, this overrides the OUI: the device named itself. */
            ev.cls = wps_hit->cls;
            ev.evidence = OBSERVORE_EVIDENCE_SSID;
            set_label(&ev, wps_hit->label);
            matched = true;
        }
        const observore_keyword_t *hit = wps_hit ? NULL :
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
        "persistence", "behaviour",
    };
    return (ev < OBSERVORE_EVIDENCE_MAX) ? names[ev] : "?";
}
