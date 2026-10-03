/* Host-side tests for the parts of Observore that do not need a radio:
 * advert parsing, signature matching, scoring and the follower heuristic.
 *
 *     make -C test && test/run
 */

#include <stdio.h>
#include <string.h>

#include "observore_detect.h"
#include "observore_version.h"
#include "observore_heapwatch.h"
#include "observore_mute.h"
#include "observore_battery.h"
#include "observore_rtc.h"
#include "observore_census.h"
#include "observore_watch.h"
#include <limits.h>
#include "observore_surroundings.h"
#include "observore_track.h"
#include "observore_notify_fmt.h"
#include "observore_util.h"
#include "observore_wps.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s:%d  " fmt "\n", __FILE__, __LINE__,            \
                   ##__VA_ARGS__);                                           \
        }                                                                    \
    } while (0)

#define SECS(n) ((int64_t)(n) * 1000000LL)

static void banner(const char *name)
{
    printf("== %s\n", name);
}

/* ---------------------------------------------------------------- */

static void test_oui_lookup(void)
{
    banner("oui lookup");

    /* B4:1E:52 is Flock Safety in the IEEE MA-L registry. */
    const uint8_t flock[6] = {0xB4, 0x1E, 0x52, 0x01, 0x02, 0x03};
    const observore_oui_t *hit = observore_oui_lookup(flock);
    CHECK(hit != NULL, "Flock Safety OUI not found");
    CHECK(hit && hit->cls == OBSERVORE_CLASS_ALPR, "Flock should classify as ALPR");

    /* 00:25:DF is Axon Enterprise. */
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0xAA, 0xBB, 0xCC};
    hit = observore_oui_lookup(axon);
    CHECK(hit != NULL, "Axon OUI not found");
    CHECK(hit && hit->cls == OBSERVORE_CLASS_BODYCAM, "Axon should classify as bodycam");

    /* A locally-administered address must never match a vendor prefix. */
    const uint8_t random_mac[6] = {0xB6, 0x1E, 0x52, 0x01, 0x02, 0x03};
    CHECK(observore_mac_is_random(random_mac), "0xB6 has the LAA bit set");
    CHECK(observore_oui_lookup(random_mac) == NULL,
          "randomised MAC must not resolve to a vendor");

    const uint8_t nobody[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    CHECK(observore_oui_lookup(nobody) == NULL, "unassigned prefix must miss");
}

static void test_vendor_lookup(void)
{
    banner("benign vendor lookup");

    /* A4:B1:97 is Apple in the IEEE MA-L registry. */
    const uint8_t apple[6] = {0xA4, 0xB1, 0x97, 0x01, 0x02, 0x03};
    const char *v = observore_vendor_lookup(apple);
    CHECK(v != NULL && strcmp(v, "Apple") == 0, "expected Apple, got '%s'",
          v ? v : "(null)");

    /* 90:41:B2 is Ubiquiti -- observed live on real access points. */
    const uint8_t ubnt[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    v = observore_vendor_lookup(ubnt);
    CHECK(v != NULL && strcmp(v, "Ubiquiti") == 0, "expected Ubiquiti, got '%s'",
          v ? v : "(null)");

    /* A randomised address carries no vendor, and a virtual BSSID with the
     * locally-administered bit set must not be attributed to whoever happens
     * to own the matching universal prefix.  9A:41:B2 is the guest-SSID BSSID
     * of a 90:41:B2 Ubiquiti radio -- observed live. */
    const uint8_t virt[6] = {0x9A, 0x41, 0xB2, 0x01, 0x02, 0x03};
    CHECK(observore_mac_is_random(virt), "0x9A has the LAA bit set");
    CHECK(observore_vendor_lookup(virt) == NULL,
          "a locally-administered BSSID must not resolve to a vendor");

    /* An unassigned prefix misses cleanly rather than returning rubbish. */
    const uint8_t nobody[6] = {0x28, 0x1F, 0x7D, 0x01, 0x02, 0x03};
    CHECK(observore_vendor_lookup(nobody) == NULL, "unassigned prefix must miss");
    CHECK(observore_vendor_lookup(NULL) == NULL, "NULL must be handled");

    /* The benign table must never classify or score.  This is the whole
     * reason it is a separate table. */
    observore_event_t ev;
    uint8_t boring[] = {0x02, 0x01, 0x06};
    observore_observation_t obs = {.mac = apple, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                               .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!observore_classify(&obs, &ev),
          "a benign vendor must not produce a detection");

    /* And a threat prefix must still win: Ring is in the threat table, so it
     * classifies even though Amazon-family kit is otherwise benign. */
    const uint8_t ring[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03};
    obs.mac = ring;
    CHECK(observore_classify(&obs, &ev), "Ring must still classify");
    CHECK(ev.cls == OBSERVORE_CLASS_CAMERA, "Ring should be a camera");
    CHECK(observore_vendor_lookup(ring) == NULL,
          "a prefix claimed as a threat must not also appear as benign");
}

static void test_vendor_labelling(void)
{
    banner("vendor labelling in the tracker");

    observore_mute_init();
    observore_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    /* An unclassified device still gets a vendor name, which is what makes it
     * recognisable enough to ignore. */
    const uint8_t apple[6] = {0xA4, 0xB1, 0x97, 0x0A, 0x0B, 0x0C};
    observore_observation_t obs = {.mac = apple, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                               .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!observore_track_observe(&obs, SECS(0)), "must not be reportable");

    observore_event_t nearby[8];
    size_t n = observore_track_nearby(nearby, 8);
    CHECK(n == 1, "expected 1 nearby device, got %zu", n);
    CHECK(n == 1 && nearby[0].vendor && strcmp(nearby[0].vendor, "Apple") == 0,
          "nearby device should be labelled Apple");

    observore_status_t st;
    observore_track_status(&st, SECS(0));
    CHECK(st.score == 0, "labelling must not score, got %u", st.score);
    CHECK(st.device_count == 0, "labelling must not create a detection");

    /* A follower gets the vendor in its label, which is the difference between
     * "something is following you" and "a Samsung is following you". */
    observore_track_observe(&obs, SECS(10));
    observore_track_observe(&obs, SECS(20));
    CHECK(observore_track_observe(&obs, SECS(400)), "follower not promoted");
    observore_event_t snap[4];
    n = observore_track_snapshot(snap, 4);
    CHECK(n == 1 && strstr(snap[0].label, "Apple") != NULL,
          "follower label should name the vendor, got '%s'",
          n ? snap[0].label : "");

    /* Nearby must exclude anything already classified. */
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 0, "a classified device must leave the nearby list, got %zu", n);
}

static void test_name_capture(void)
{
    banner("name and ssid capture");

    observore_mute_init();
    observore_track_init();

    /* An unclassified BLE device that broadcasts a name must carry it, even
     * though nothing about it classified. */
    const uint8_t mac[6] = {0xA4, 0xB1, 0x97, 0x01, 0x02, 0x03};
    uint8_t named[] = {0x02, 0x01, 0x06,
                       0x06, 0x09, 'S', 'h', 'e', 'l', 'f'};
    observore_observation_t obs = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                               .adv = named, .adv_len = sizeof(named)};
    CHECK(!observore_track_observe(&obs, SECS(0)), "must not classify");

    observore_event_t nearby[8];
    size_t n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "name not captured, got '%s'", n ? nearby[0].detail : "");

    /* BLE names often arrive in a scan response, several sightings after the
     * device first appears.  A later nameless advert from the same address
     * must not wipe the name we already learned. */
    uint8_t nameless[] = {0x02, 0x01, 0x06};
    obs.adv = nameless;
    obs.adv_len = sizeof(nameless);
    observore_track_observe(&obs, SECS(5));
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "a nameless advert erased the learned name, got '%s'",
          n ? nearby[0].detail : "");

    /* And the reverse: a name arriving late is picked up. */
    observore_track_init();
    obs.adv = nameless;
    obs.adv_len = sizeof(nameless);
    observore_track_observe(&obs, SECS(0));
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && nearby[0].detail[0] == '\0', "should start nameless");
    obs.adv = named;
    obs.adv_len = sizeof(named);
    observore_track_observe(&obs, SECS(1));
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "a late name was not learned, got '%s'", n ? nearby[0].detail : "");

    /* Wi-Fi carries the SSID the same way. */
    observore_track_init();
    const uint8_t ap[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    observore_observation_t wifi = {.mac = ap, .src = OBSERVORE_SRC_WIFI_SCAN,
                                .rssi = -50, .ssid = "42-Guest"};
    observore_track_observe(&wifi, SECS(0));
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && strcmp(nearby[0].detail, "42-Guest") == 0,
          "ssid not captured, got '%s'", n ? nearby[0].detail : "");
    CHECK(n == 1 && nearby[0].vendor && strcmp(nearby[0].vendor, "Ubiquiti") == 0,
          "vendor should still resolve alongside the ssid");

    /* A hidden AP reports an empty SSID and must simply stay nameless. */
    observore_track_init();
    wifi.ssid = "";
    observore_track_observe(&wifi, SECS(0));
    n = observore_track_nearby(nearby, 8);
    CHECK(n == 1 && nearby[0].detail[0] == '\0',
          "a hidden AP should stay nameless, got '%s'",
          n ? nearby[0].detail : "");
}

static void test_adv_parsing(void)
{
    banner("advert parsing");

    /* len,type,value ... with a complete local name. */
    const uint8_t adv[] = {
        0x02, 0x01, 0x06,                          /* flags */
        0x06, 0x09, 'A', 'x', 'o', 'n', 'X',       /* complete local name */
    };
    char name[24];
    CHECK(observore_adv_name(adv, sizeof(adv), name, sizeof(name)), "name not parsed");
    CHECK(strcmp(name, "AxonX") == 0, "got name '%s'", name);

    /* A field claiming more bytes than remain must not read past the end. */
    const uint8_t truncated[] = {0x02, 0x01, 0x06, 0x20, 0x09, 'A'};
    CHECK(!observore_adv_name(truncated, sizeof(truncated), name, sizeof(name)),
          "over-long field must be rejected, not parsed");

    /* Zero-length field terminates the walk rather than looping forever. */
    const uint8_t padded[] = {0x02, 0x01, 0x06, 0x00, 0x00, 0x00};
    size_t len = 0;
    CHECK(observore_adv_field(padded, sizeof(padded), 0x09, &len) == NULL,
          "padding must not yield a field");

    /* Non-printable bytes in a name are sanitised, not passed through. */
    const uint8_t nasty[] = {0x05, 0x09, 'a', 0x00, 0x1B, 'b'};
    CHECK(observore_adv_name(nasty, sizeof(nasty), name, sizeof(name)), "name missing");
    CHECK(strcmp(name, "a..b") == 0, "unsanitised name '%s'", name);
}

static bool classify_ble(const uint8_t *mac, const uint8_t *adv, size_t adv_len,
                         observore_event_t *out)
{
    observore_observation_t obs = {
        .mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -50,
        .adv = adv, .adv_len = adv_len,
    };
    return observore_classify(&obs, out);
}

static void test_ble_signatures(void)
{
    banner("ble signatures");

    const uint8_t mac[6] = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55}; /* random */
    observore_event_t ev;

    /* Apple Find My separated broadcast: FF 4C00 12 19 <status> <key...> */
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E;          /* field length */
    findmy[1] = 0xFF;          /* manufacturer specific data */
    findmy[2] = 0x4C;
    findmy[3] = 0x00;          /* Apple, little-endian */
    findmy[4] = 0x12;          /* Find My payload type */
    findmy[5] = 0x19;          /* payload length */
    findmy[6] = 0x10;          /* status: AirTag, full battery */
    CHECK(classify_ble(mac, findmy, sizeof(findmy), &ev), "Find My not detected");
    CHECK(ev.cls == OBSERVORE_CLASS_TRACKER, "Find My should be a tracker");
    CHECK(ev.evidence == OBSERVORE_EVIDENCE_MFG_DATA, "expected mfg-data evidence");

    /* An Apple advert of a different payload type is ordinary Apple traffic
     * and must not be reported -- every iPhone in the room emits these. */
    uint8_t nearby[10] = {0x09, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0, 0, 0, 0};
    CHECK(!classify_ble(mac, nearby, sizeof(nearby), &ev),
          "Apple Nearby Info must not be flagged as a tracker");

    /* OpenDroneID: service data under 0xFFFA opening with app code 0x0D. */
    uint8_t odid[] = {0x06, 0x16, 0xFA, 0xFF, 0x0D, 0x00, 0x01};
    CHECK(classify_ble(mac, odid, sizeof(odid), &ev), "Remote ID not detected");
    CHECK(ev.cls == OBSERVORE_CLASS_DRONE, "Remote ID should be a drone");

    /* Same UUID, different ASTM application -- not a drone. */
    uint8_t not_odid[] = {0x06, 0x16, 0xFA, 0xFF, 0x01, 0x00, 0x01};
    CHECK(!classify_ble(mac, not_odid, sizeof(not_odid), &ev),
          "non-ODID ASTM service data must not be flagged");

    /* Tile advertises service UUID 0xFEED. */
    uint8_t tile[] = {0x03, 0x03, 0xED, 0xFE};
    CHECK(classify_ble(mac, tile, sizeof(tile), &ev), "Tile not detected");
    CHECK(ev.cls == OBSERVORE_CLASS_TRACKER, "Tile should be a tracker");

    /* Name keyword fallback. */
    uint8_t named[] = {0x09, 0x09, 'F', 'l', 'o', 'c', 'k', '-', '1', '2'};
    CHECK(classify_ble(mac, named, sizeof(named), &ev), "name keyword missed");
    CHECK(ev.cls == OBSERVORE_CLASS_ALPR, "Flock name should be ALPR");
    CHECK(ev.evidence == OBSERVORE_EVIDENCE_NAME, "expected name evidence");

    /* A bare Samsung company ID must NOT be reported.  Every Samsung phone,
     * watch and earbud advertises 0x0075; matching the vendor alone turns a
     * crowded room into a wall of phantom trackers.  Observed live. */
    uint8_t samsung[] = {0x05, 0xFF, 0x75, 0x00, 0x42, 0x01, 0x00};
    CHECK(!classify_ble(mac, samsung, sizeof(samsung), &ev),
          "bare Samsung company ID must not be flagged as a tracker");

    /* The actual SmartTag signature is its 0xFD5A service data. */
    uint8_t smarttag[] = {0x05, 0x16, 0x5A, 0xFD, 0x01, 0x02};
    CHECK(classify_ble(mac, smarttag, sizeof(smarttag), &ev), "SmartTag missed");
    CHECK(ev.cls == OBSERVORE_CLASS_TRACKER, "SmartTag should be a tracker");

    /* Nothing at all. */
    uint8_t boring[] = {0x02, 0x01, 0x06};
    CHECK(!classify_ble(mac, boring, sizeof(boring), &ev),
          "a bare flags advert must not match");

    /* A payload signature must win over a vendor prefix, because trackers
     * rotate MACs and the prefix is the weaker evidence. */
    const uint8_t ring_mac[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03}; /* Ring */
    CHECK(classify_ble(ring_mac, findmy, sizeof(findmy), &ev), "no match");
    CHECK(ev.cls == OBSERVORE_CLASS_TRACKER,
          "Find My payload should override the Ring OUI");
}

static void test_random_address(void)
{
    banner("ble random addresses");

    /* A random BLE address that happens to collide with a real vendor prefix
     * must not be attributed to that vendor.  BLE reports the address type on
     * the wire, so there is no need to guess from the MAC bits. */
    const uint8_t looks_like_ring[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    observore_observation_t obs = {
        .mac = looks_like_ring, .src = OBSERVORE_SRC_BLE, .rssi = -50,
        .addr_random = true, .adv = boring, .adv_len = sizeof(boring),
    };
    observore_event_t ev;
    CHECK(!observore_classify(&obs, &ev),
          "a random address must not resolve to a vendor prefix");

    /* The same address reported as public does resolve. */
    obs.addr_random = false;
    CHECK(observore_classify(&obs, &ev), "public address should match the OUI");
    CHECK(ev.cls == OBSERVORE_CLASS_CAMERA, "should be a camera");
}

static void test_random_signals_disagree(void)
{
    banner("random-address signals disagreeing");

    /* Measured on real air: the controller reports some addresses as public
     * whose locally-administered bit is set, and some as random whose bit is
     * clear.  Either signal alone mislabels half the traffic. */
    uint8_t boring[] = {0x02, 0x01, 0x06};

    /* Controller says public, LAA bit says random (CB:1F:FE, observed). */
    const uint8_t laa_only[6] = {0xCB, 0x1F, 0xFE, 0x3B, 0xB1, 0xAA};
    observore_observation_t a = {.mac = laa_only, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .addr_random = false,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(observore_obs_is_random(&a), "LAA bit alone must count as random");

    /* Controller says random, LAA bit says universal (20:7C:3A, observed). */
    const uint8_t type_only[6] = {0x20, 0x7C, 0x3A, 0x7D, 0x1A, 0x20};
    observore_observation_t b = {.mac = type_only, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .addr_random = true,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(observore_obs_is_random(&b), "the transport flag alone must count too");
    CHECK(!observore_mac_is_random(type_only),
          "this address's LAA bit is clear -- that is the point of the test");

    /* A genuinely public address is still usable. */
    const uint8_t real[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    observore_observation_t c = {.mac = real, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .addr_random = false,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!observore_obs_is_random(&c), "a public address must stay usable");

    /* The flag reaches the tracker, so the UI can say "random MAC" rather
     * than "unknown vendor" -- different facts. */
    observore_mute_init();
    observore_track_init();
    observore_track_observe(&a, SECS(0));
    observore_event_t nearby[4];
    size_t n = observore_track_nearby(nearby, 4);
    CHECK(n == 1 && nearby[0].addr_random,
          "tracker should record the address as random");
    CHECK(n == 1 && nearby[0].vendor == NULL,
          "a random address must not carry a vendor");

    CHECK(observore_obs_is_random(NULL), "NULL must be treated as unknowable");
}

static void test_ssid(void)
{
    banner("ssid keywords");

    char label[24];
    CHECK(observore_ssid_is_suspicious("FlockSafety_Falcon", label, sizeof(label)),
          "Flock SSID missed");
    CHECK(observore_ssid_is_suspicious("lobby-CCTV-2", label, sizeof(label)),
          "CCTV SSID missed");
    CHECK(!observore_ssid_is_suspicious("Starbucks WiFi", label, sizeof(label)),
          "ordinary SSID must not match");
    CHECK(!observore_ssid_is_suspicious("", label, sizeof(label)), "empty SSID");
}

static void test_follower(void)
{
    banner("follower heuristic");

    observore_track_init();
    const uint8_t mac[6] = {0x4A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    observore_observation_t obs = {
        .mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -60,
        .adv = boring, .adv_len = sizeof(boring),
    };

    /* Three sightings inside a minute are a device sitting nearby, not one
     * following you -- the span requirement is what separates them. */
    CHECK(!observore_track_observe(&obs, SECS(0)), "first sighting is not a hit");
    CHECK(!observore_track_observe(&obs, SECS(10)), "second sighting is not a hit");
    CHECK(!observore_track_observe(&obs, SECS(20)), "3 hits in 20s must not trip");

    /* Same address still around five minutes later: that is a follower. */
    CHECK(observore_track_observe(&obs, SECS(301)), "follower not promoted");

    observore_status_t st;
    observore_track_status(&st, SECS(301));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 1, "follower not counted");
    /* Presence alone is worth a point, not the class weight: this device has
     * been in the room five minutes and has not rotated its address, which is
     * what everybody in a cafe looks like. */
    CHECK(st.score == OBSERVORE_FOLLOWER_PRESENT_POINTS,
          "mere presence should score %u, got %u",
          OBSERVORE_FOLLOWER_PRESENT_POINTS, st.score);
}






static void test_the_watch_hands_point_the_right_way(void)
{
    banner("a dial hand at twelve points up, not sideways");

    int x = 0, y = 0;
    /* Twelve o'clock is straight up, which on a screen is negative y. An
     * off-by-a-quarter-turn here is the classic way to ship a clock that is
     * ninety degrees out and looks plausible in a photograph. */
    observore_watch_hand_end(100, 100, 50, 0, 12, &x, &y);
    CHECK(x == 100 && y == 50, "twelve is straight up, got %d,%d", x, y);
    observore_watch_hand_end(100, 100, 50, 3, 12, &x, &y);
    CHECK(x == 150 && y == 100, "three is to the right, got %d,%d", x, y);
    observore_watch_hand_end(100, 100, 50, 6, 12, &x, &y);
    CHECK(x == 100 && y == 150, "six is down, got %d,%d", x, y);
    observore_watch_hand_end(100, 100, 50, 9, 12, &x, &y);
    CHECK(x == 50 && y == 100, "nine is to the left, got %d,%d", x, y);

    /* Seconds use the same arithmetic with sixty to the turn. */
    observore_watch_hand_end(0, 0, 100, 15, 60, &x, &y);
    CHECK(x == 100 && y == 0, "fifteen seconds is to the right, got %d,%d", x, y);
    observore_watch_hand_end(0, 0, 100, 30, 60, &x, &y);
    CHECK(x == 0 && y == 100, "thirty seconds is down, got %d,%d", x, y);

    /* Halfway between hours, which is where a real hour hand sits at half
     * past and where a naive integer division puts it on the hour instead. */
    observore_watch_hand_end(0, 0, 1000, 90, 720, &x, &y);
    CHECK(x > 690 && x < 720 && y > -720 && y < -690,
          "half past one is between one and two, got %d,%d", x, y);

    /* Nothing may be written outside the canvas, whatever it is asked to
     * draw: a hand longer than the dial is a mistake that should clip rather
     * than corrupt whatever follows the buffer. */
    static uint16_t px[32 * 32];
    observore_canvas_t c = {.px = px, .w = 32, .h = 32};
    observore_watch_fill(&c, 0x0000);
    observore_watch_line(&c, 16, 16, 500, -400, 5, 0xFFFF);
    observore_watch_disc(&c, 2, 2, 40, 0xFFFF);
    observore_watch_ring(&c, 16, 16, 60, 4, 0xFFFF);
    observore_watch_text(&c, 16, 16, "XII", 3, 0xFFFF);
    CHECK(1, "drawing past the edges does not corrupt memory");
}

static void test_the_dial_wanders_without_leaving_the_glass(void)
{
    banner("the dial walks so the panel does not keep it");

    /* The real geometry, and the radius from the same function the display
     * calls -- not a copy of the sum. A test that recomputed the radius here
     * would pass while the display forgot to subtract the walk, which is the
     * one mistake this check exists to catch. */
    const int panel = 466, margin = 4, shift = 3, ring = 5;
    const int r = observore_watch_dial_radius(panel, panel, margin, ring, shift);

    int seen_x[8], seen_y[8];
    int distinct = 0;
    for (unsigned step = 0; step < 8; step++) {
        int dx, dy;
        observore_watch_shift(step, shift, &dx, &dy);
        seen_x[step] = dx;
        seen_y[step] = dy;

        /* Inside the circle it is allowed to wander in. */
        CHECK(dx * dx + dy * dy <= shift * shift + 1,
              "step stays within the walk radius");

        /* And the dial, at that offset, still keeping the margin the design
         * asked for -- not merely inside the panel.
         *
         * "On the glass" is too weak to be worth asserting: with four pixels
         * of margin to spend, a dial that had not paid for the walk at all
         * would still fit, by one pixel, and the check would pass while the
         * ring sat on the bezel. The margin is the rule, so the margin is
         * what gets checked. */
        int reach = r + (ring + 1) / 2;
        CHECK(panel / 2 + dx + reach <= panel - margin, "keeps its margin at the right");
        CHECK(panel / 2 + dx - reach >= margin,         "keeps its margin at the left");
        CHECK(panel / 2 + dy + reach <= panel - margin, "keeps its margin at the bottom");
        CHECK(panel / 2 + dy - reach >= margin,         "keeps its margin at the top");

        bool dup = false;
        for (int i = 0; i < distinct; i++) {
            if (seen_x[i] == dx && seen_y[i] == dy) { dup = true; break; }
        }
        if (!dup) { distinct++; }
    }
    /* A walk that visits two pixels is not a walk. Eight points on a circle
     * of radius three round to six distinct offsets, because the diagonals
     * land on the same pixel pair as nothing else does -- what matters is
     * that it is most of them rather than one. */
    CHECK(distinct >= 6, "the walk visits most of its eight positions");

    /* Consecutive steps are adjacent. This is what a circle buys over a
     * raster scan: no step crosses the face, so there is no one moment a
     * minute when the dial visibly jumps. */
    for (unsigned step = 0; step < 8; step++) {
        int ax, ay, bx, by;
        observore_watch_shift(step, shift, &ax, &ay);
        observore_watch_shift(step + 1, shift, &bx, &by);
        int d2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
        CHECK(d2 <= 2 * shift * shift,
              "the dial moves to a neighbouring position, never across");
    }

    /* It cycles rather than running off, so step counts from the clock are
     * safe for as long as the device runs. */
    int ax, ay, bx, by;
    observore_watch_shift(3, shift, &ax, &ay);
    observore_watch_shift(3 + 8, shift, &bx, &by);
    CHECK(ax == bx && ay == by, "the walk repeats every eight steps");
    observore_watch_shift(3 + 8 * 1000000, shift, &bx, &by);
    CHECK(ax == bx && ay == by, "and still does a million cycles later");

    /* The radius pays for the walk. Asking for a wider walk must give back a
     * smaller dial, or the ring leaves the glass -- and the edge checks above
     * only catch that because this is the function the display uses. */
    CHECK(observore_watch_dial_radius(panel, panel, margin, ring, shift) <
          observore_watch_dial_radius(panel, panel, margin, ring, 0),
          "a walk costs radius");
    CHECK(observore_watch_dial_radius(panel, panel, margin, ring, 0) ==
          panel / 2 - margin - (ring + 1) / 2,
          "no walk leaves the dial where it always was, less its own ring");
    CHECK(observore_watch_dial_radius(100, 400, 4, 5, 3) ==
          observore_watch_dial_radius(100, 100, 4, 5, 3),
          "the short side decides on a panel that is not square");
    CHECK(observore_watch_dial_radius(20, 20, 4, 5, 300) == 0,
          "a walk wider than the panel gives no dial rather than a negative one");

    /* Asked for no walk, it stands still -- which is what every board
     * without an AMOLED would want if this were ever shared. */
    observore_watch_shift(5, 0, &ax, &ay);
    CHECK(ax == 0 && ay == 0, "radius zero does not move the dial");
    observore_watch_shift(5, -3, &ax, &ay);
    CHECK(ax == 0 && ay == 0, "a negative radius is treated as none");
}

static void test_the_census_earns_membership_over_days(void)
{
    banner("the census counts days, not sightings");

    /* Days from 2000-01-01, against dates worked out independently. The
     * arithmetic is written out rather than taken from mktime(), so it is
     * worth pinning to real dates including the awkward ones. */
    struct tm t = {0};
    t.tm_year = 100; t.tm_mon = 0; t.tm_mday = 1;     /* 2000-01-01 */
    CHECK(observore_census_day_from_tm(&t) == 0, "2000-01-01 is day zero");
    t.tm_mday = 2;
    CHECK(observore_census_day_from_tm(&t) == 1, "and the next day is one");
    t.tm_mon = 2; t.tm_mday = 1;                      /* 2000-03-01 */
    CHECK(observore_census_day_from_tm(&t) == 60, "2000 was a leap year");
    t.tm_year = 101; t.tm_mon = 0; t.tm_mday = 1;     /* 2001-01-01 */
    CHECK(observore_census_day_from_tm(&t) == 366, "a leap year is 366 days");
    t.tm_year = 124; t.tm_mon = 1; t.tm_mday = 29;    /* 2024-02-29 */
    CHECK(observore_census_day_from_tm(&t) == 8825, "2024-02-29 exists and is 8825");
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 3;     /* 2026-10-03 */
    CHECK(observore_census_day_from_tm(&t) == 9772, "today is 9772");

    /* A clock that has not been set has no day. ESP-IDF starts at 1970 and
     * every sighting before the first SNTP reply lands there, so this is the
     * normal state for the first minute of every run rather than an edge. */
    t.tm_year = 70; t.tm_mon = 0; t.tm_mday = 1;      /* 1970-01-01 */
    CHECK(observore_census_day_from_tm(&t) == OBSERVORE_CENSUS_NO_DAY,
          "1970 is an unset clock, not a date");
    CHECK(observore_census_day_from_tm(NULL) == OBSERVORE_CENSUS_NO_DAY,
          "no date at all is no day");

    /* One evening is one day, however many sightings it holds. This is the
     * whole point of the rule: a device at your elbow all evening is
     * interesting precisely because it is not furniture. */
    observore_census_init();
    for (int i = 0; i < 500; i++) {
        observore_census_note(0xAABBCCDD, 9000);
    }
    CHECK(observore_census_days_seen(0xAABBCCDD, 9000) == 1,
          "five hundred sightings in one evening are one day");
    CHECK(!observore_census_is_household(0xAABBCCDD, 9000),
          "and one day is not furniture");

    /* Three separate days are. */
    observore_census_init();
    observore_census_note(0x1111, 9000);
    CHECK(!observore_census_is_household(0x1111, 9000), "one day: no");
    observore_census_note(0x1111, 9001);
    CHECK(!observore_census_is_household(0x1111, 9001), "two days: no");
    observore_census_note(0x1111, 9002);
    CHECK(observore_census_is_household(0x1111, 9002), "three days: furniture");
    CHECK(observore_census_days_seen(0x1111, 9002) == 3, "and it says three");

    /* Days need not be consecutive -- a printer nobody uses at the weekend is
     * still furniture. */
    observore_census_init();
    observore_census_note(0x2222, 9000);
    observore_census_note(0x2222, 9004);
    observore_census_note(0x2222, 9008);
    CHECK(observore_census_is_household(0x2222, 9008), "spread across a week still counts");

    /* Decay, which the bitmap does by itself: a visitor's phone seen three
     * days running stops being household once those days fall out of the
     * window. */
    observore_census_init();
    observore_census_note(0x3333, 9000);
    observore_census_note(0x3333, 9001);
    observore_census_note(0x3333, 9002);
    CHECK(observore_census_is_household(0x3333, 9002), "household the day they leave");
    CHECK(observore_census_is_household(0x3333, 9010), "and still, a week later");
    CHECK(!observore_census_is_household(0x3333, 9002 + OBSERVORE_CENSUS_WINDOW),
          "but not once the window has passed over it");
    CHECK(observore_census_days_seen(0x3333, 9002 + OBSERVORE_CENSUS_WINDOW) == 0,
          "and nothing is remembered of it");

    /* Sightings with no date are dropped rather than counted against day
     * zero, which would hand membership to whatever was in the room during
     * the first minute after every boot. */
    observore_census_init();
    for (int i = 0; i < 10; i++) {
        observore_census_note(0x4444, OBSERVORE_CENSUS_NO_DAY);
    }
    int tracked = -1, tracked2 = -1;
    observore_census_counts(9000, NULL, &tracked);
    CHECK(tracked == 0, "an undated sighting records nothing at all");

    /* The clock jumping forward by nine thousand days. This is not a
     * hypothetical: it happens on every run, when SNTP answers and the day
     * number goes from unset to real. A shift of sixteen or more on a
     * uint16_t is undefined behaviour rather than zero, so the distance is
     * bounded before it is used. */
    observore_census_init();
    observore_census_note(0x5555, 10);
    observore_census_note(0x5555, 11);
    observore_census_note(0x5555, 12);
    CHECK(observore_census_is_household(0x5555, 12), "household before the jump");
    CHECK(observore_census_days_seen(0x5555, 30000) == 0,
          "a jump wider than the window clears the mask rather than wrapping it");
    observore_census_note(0x5555, 30000);
    CHECK(observore_census_days_seen(0x5555, 30000) == 1,
          "and starts again from one day, not four");

    /* A day the record cannot hold is refused rather than truncated. Sixteen
     * bits run out in 2179, and a truncated day number is not a near miss: it
     * claims a different date, and the window is then measured from it. This
     * is here because the first version of this test asked about day 100000
     * and got a confident wrong answer. */
    observore_census_init();
    observore_census_note(0x5556, OBSERVORE_CENSUS_DAY_MAX + 1);
    observore_census_counts(9000, NULL, &tracked2);
    CHECK(tracked2 == 0, "a date past 2179 records nothing rather than wrapping");
    observore_census_note(0x5556, 9000);
    CHECK(observore_census_days_seen(0x5556, OBSERVORE_CENSUS_DAY_MAX + 1) == 0,
          "and asking about one answers nothing rather than guessing");
    t.tm_year = 400; t.tm_mon = 0; t.tm_mday = 1;     /* 2300-01-01 */
    CHECK(observore_census_day_from_tm(&t) == OBSERVORE_CENSUS_NO_DAY,
          "a date beyond the record has no day number");

    /* A clock corrected backwards must not rewrite history it cannot
     * reconstruct. */
    observore_census_init();
    observore_census_note(0x6666, 9000);
    observore_census_note(0x6666, 9001);
    observore_census_note(0x6666, 9002);
    observore_census_note(0x6666, 8990);   /* the clock steps back */
    CHECK(observore_census_days_seen(0x6666, 9002) == 3,
          "a backwards step does not shift the window the wrong way");
    CHECK(observore_census_is_household(0x6666, 9002), "and membership survives it");

    /* Asking a question must not change the answer to the next one. */
    observore_census_init();
    observore_census_note(0x7777, 9000);
    observore_census_note(0x7777, 9001);
    observore_census_note(0x7777, 9002);
    (void)observore_census_days_seen(0x7777, 99999);   /* ages a copy, not the entry */
    CHECK(observore_census_days_seen(0x7777, 9002) == 3,
          "asking about the distant future does not erase what is known");

    /* An unknown id is not furniture and is not an error. */
    CHECK(!observore_census_is_household(0xDEAD, 9002), "an unknown device is not household");
    CHECK(observore_census_days_seen(0xDEAD, 9002) == 0, "and has no days");

    /* The table fills, and what goes is the oldest -- which on a table this
     * size is the thing least likely to be furniture, because anything
     * around every day is among the most recently seen. */
    observore_census_init();
    for (int i = 0; i < OBSERVORE_CENSUS_MAX; i++) {
        observore_census_note(0x8000u + (uint32_t)i, 9000 + i);
    }
    observore_census_counts(9100, NULL, &tracked);
    CHECK(tracked == OBSERVORE_CENSUS_MAX, "the table fills to its limit");
    observore_census_note(0x9999, 9200);
    observore_census_counts(9200, NULL, &tracked);
    CHECK(tracked == OBSERVORE_CENSUS_MAX, "and does not grow past it");
    CHECK(observore_census_days_seen(0x9999, 9200) == 1, "the newcomer is recorded");
    CHECK(observore_census_days_seen(0x8000u, 9200) == 0, "the oldest was the one evicted");
    CHECK(observore_census_days_seen(0x8000u + OBSERVORE_CENSUS_MAX - 1, 9200) != 0 ||
          OBSERVORE_CENSUS_MAX - 1 >= OBSERVORE_CENSUS_WINDOW,
          "the most recent was not");

    /* Counts, for the console: how many are furniture and how many are merely
     * known. The difference is the thing worth watching while nothing acts on
     * this yet. */
    observore_census_init();
    observore_census_note(0xA1, 9000);
    observore_census_note(0xA1, 9001);
    observore_census_note(0xA1, 9002);   /* furniture */
    observore_census_note(0xA2, 9002);   /* merely seen */
    int household = -1;
    observore_census_counts(9002, &household, &tracked);
    CHECK(tracked == 2 && household == 1, "two known, one of them furniture");

    /* Persistence. The blob is the table, so the check that matters is that a
     * blob which is not a whole number of entries is refused rather than read
     * as garbage -- the same trap the mute store was fixed for. */
    const observore_census_entry_t *tab = NULL;
    size_t n = observore_census_entries(&tab);
    CHECK(n == 2 && tab != NULL, "the table is readable for saving");
    observore_census_entry_t saved[2];
    memcpy(saved, tab, sizeof(saved));

    observore_census_init();
    observore_census_counts(9002, NULL, &tracked);
    CHECK(tracked == 0, "cleared");
    CHECK(observore_census_restore(saved, sizeof(saved)), "a whole table restores");
    CHECK(observore_census_is_household(0xA1, 9002), "membership survived the round trip");
    CHECK(observore_census_days_seen(0xA2, 9002) == 1, "and so did the partial record");

    CHECK(!observore_census_restore(saved, sizeof(saved) - 1), "half an entry is refused");
    CHECK(!observore_census_restore(saved, 0), "an empty blob is refused");
    CHECK(!observore_census_restore(NULL, sizeof(saved)), "no blob is refused");
    static observore_census_entry_t toobig[OBSERVORE_CENSUS_MAX + 1];
    CHECK(!observore_census_restore(toobig, sizeof(toobig)),
          "a blob longer than the table is refused rather than truncated");
    CHECK(observore_census_is_household(0xA1, 9002),
          "and a refused restore leaves what was there alone");
}

static void test_bcd(void)
{
    banner("the clock chip speaks BCD and nothing else here does");

    CHECK(observore_bcd_to_dec(0x00) == 0, "0x00 is 0");
    CHECK(observore_bcd_to_dec(0x09) == 9, "0x09 is 9");
    CHECK(observore_bcd_to_dec(0x10) == 10, "0x10 is ten, not sixteen");
    CHECK(observore_bcd_to_dec(0x59) == 59, "0x59 is 59");
    CHECK(observore_dec_to_bcd(0) == 0x00, "0 is 0x00");
    CHECK(observore_dec_to_bcd(9) == 0x09, "9 is 0x09");
    CHECK(observore_dec_to_bcd(10) == 0x10, "ten is 0x10, not 0x0A");
    CHECK(observore_dec_to_bcd(59) == 0x59, "59 is 0x59");

    /* And the epoch arithmetic that replaced a missing timegm(). Checked
     * against dates computed elsewhere rather than against itself. */
    struct tm t0 = {.tm_year = 70, .tm_mon = 0, .tm_mday = 1};
    CHECK(observore_timegm(&t0) == 0, "1970-01-01 is zero, got %lld",
          (long long)observore_timegm(&t0));
    struct tm t1 = {.tm_year = 126, .tm_mon = 8, .tm_mday = 29,
                    .tm_hour = 12, .tm_min = 34, .tm_sec = 56};
    CHECK(observore_timegm(&t1) == 1790685296LL,
          "2026-09-29 12:34:56 UTC, got %lld", (long long)observore_timegm(&t1));
    /* A leap day, which is where this arithmetic earns its keep. */
    struct tm t2 = {.tm_year = 124, .tm_mon = 1, .tm_mday = 29};
    struct tm t3 = {.tm_year = 124, .tm_mon = 2, .tm_mday = 1};
    CHECK(observore_timegm(&t3) - observore_timegm(&t2) == 86400,
          "2024-02-29 is one day before 2024-03-01");
    /* And a century that is not a leap year. */
    struct tm t4 = {.tm_year = 200, .tm_mon = 1, .tm_mday = 28};
    struct tm t5 = {.tm_year = 200, .tm_mon = 2, .tm_mday = 1};
    CHECK(observore_timegm(&t5) - observore_timegm(&t4) == 86400,
          "2100 is not a leap year");

    /* Round trip across every value the chip can hold, in one check. */
    int bad = -1;
    for (int i = 0; i < 100 && bad < 0; i++) {
        if (observore_bcd_to_dec(observore_dec_to_bcd((uint8_t)i)) != i) {
            bad = i;
        }
    }
    CHECK(bad < 0, "every value 0-99 survives the round trip (failed at %d)", bad);
}

static void test_the_battery_curve(void)
{
    banner("charge from voltage, on a curve rather than a straight line");

    CHECK(observore_battery_pct_from_mv(4200) == 100, "4.20 V is full");
    CHECK(observore_battery_pct_from_mv(4500) == 100, "and above it is still full");
    CHECK(observore_battery_pct_from_mv(3300) == 0, "3.30 V is empty");
    CHECK(observore_battery_pct_from_mv(3000) == 0, "and below it is still empty");
    CHECK(observore_battery_pct_from_mv(-1) == -1, "no reading is not zero percent");

    /* The flat middle is the whole reason this is a table. A cell at 3.80 V
     * is a little under half, where a straight line from 3.3 to 4.2 would
     * call it 55% -- optimistic by a tenth of a battery, in the span where a
     * battery spends most of its life. */
    int mid = observore_battery_pct_from_mv(3800);
    CHECK(mid > 35 && mid < 50, "3.80 V is a little under half, got %d%%", mid);
    int linear = (3800 - 3300) * 100 / (4200 - 3300);
    CHECK(linear > mid + 5,
          "and a straight line would have flattered it (%d%% vs %d%%)",
          linear, mid);

    /* Monotonic, which a hand-written table is not automatically. One check
     * over the whole sweep rather than ninety, naming the first place it
     * would go backwards. */
    int prev = -1, bad_mv = 0;
    for (int mv = 3300; mv <= 4200 && !bad_mv; mv += 10) {
        int pct = observore_battery_pct_from_mv(mv);
        if (pct < prev) {
            bad_mv = mv;
        }
        prev = pct;
    }
    CHECK(bad_mv == 0, "charge must never fall as voltage rises (fell at %d mV)",
          bad_mv);
}

static void test_a_journey_must_change_the_room(void)
{
    banner("a journey that did not go anywhere is not a journey");

    /* Access points are the reference frame: stationary, plentiful, and
     * already scanned. Carrying the board around one building leaves them
     * all in earshot, which is exactly what made every follower in a house
     * look like it had come along. */
    observore_surroundings_reset();
    for (uint32_t ap = 1; ap <= 9; ap++) {
        observore_surroundings_note(ap, -50, SECS(0));
    }
    observore_surroundings_mark(SECS(1), SECS(300));
    CHECK(observore_surroundings_marked() == 9, "nine access points marked, got %u",
          (unsigned)observore_surroundings_marked());
    CHECK(observore_surroundings_overlap_pct() == 0,
          "none heard again yet, got %d", observore_surroundings_overlap_pct());

    /* Walked to the kitchen: the same building answers. */
    for (uint32_t ap = 1; ap <= 8; ap++) {
        observore_surroundings_note(ap, -50, SECS(60));
    }
    CHECK(observore_surroundings_overlap_pct() >= 80,
          "the same place shares nearly all of them, got %d%%",
          observore_surroundings_overlap_pct());

    /* Went somewhere else: almost nothing from before is audible. */
    observore_surroundings_reset();
    for (uint32_t ap = 1; ap <= 9; ap++) {
        observore_surroundings_note(ap, -50, SECS(0));
    }
    observore_surroundings_mark(SECS(1), SECS(300));
    observore_surroundings_note(1, -50, SECS(600));          /* one straggler */
    for (uint32_t ap = 100; ap <= 112; ap++) {
        observore_surroundings_note(ap, -50, SECS(600));     /* all new */
    }
    int elsewhere = observore_surroundings_overlap_pct();
    CHECK(elsewhere > 0 && elsewhere <= 33,
          "somewhere else shares almost none, got %d%%", elsewhere);

    /* The countryside case, which membership alone gets wrong. Out where the
     * only access points are your own house, a barn down the driveway keeps
     * most of them -- 80% here, measured on real hardware -- and the journey
     * did not count. The same access points, all of them much fainter, is
     * distance. */
    observore_surroundings_reset();
    for (uint32_t ap = 1; ap <= 5; ap++) {
        observore_surroundings_note(ap, -45, SECS(0));
    }
    observore_surroundings_mark(SECS(1), SECS(300));
    for (uint32_t ap = 1; ap <= 4; ap++) {
        observore_surroundings_note(ap, -74, SECS(600));   /* 29 dB fainter */
    }
    CHECK(observore_surroundings_overlap_pct() >= 75,
          "the barn still hears the house, got %d%%",
          observore_surroundings_overlap_pct());
    CHECK(observore_surroundings_faded_db() == 29,
          "but they are 29 dB fainter, got %d",
          observore_surroundings_faded_db());

    /* Shifting the thing on a desk must not read as a journey. */
    observore_surroundings_reset();
    for (uint32_t ap = 1; ap <= 5; ap++) {
        observore_surroundings_note(ap, -50, SECS(0));
    }
    observore_surroundings_mark(SECS(1), SECS(300));
    const int jitter[5] = {-53, -47, -50, -52, -48};
    for (uint32_t ap = 1; ap <= 5; ap++) {
        observore_surroundings_note(ap, jitter[ap - 1], SECS(60));
    }
    CHECK(observore_surroundings_faded_db() <= 3,
          "ordinary jitter is not distance, got %d dB",
          observore_surroundings_faded_db());

    /* One access point behind a tractor cannot carry the answer: the median
     * ignores it where a mean would not. */
    observore_surroundings_reset();
    for (uint32_t ap = 1; ap <= 5; ap++) {
        observore_surroundings_note(ap, -50, SECS(0));
    }
    observore_surroundings_mark(SECS(1), SECS(300));
    observore_surroundings_note(1, -95, SECS(60));      /* one lost */
    for (uint32_t ap = 2; ap <= 5; ap++) {
        observore_surroundings_note(ap, -50, SECS(60));
    }
    CHECK(observore_surroundings_faded_db() == 0,
          "four unchanged outvote one, got %d dB",
          observore_surroundings_faded_db());

    /* Too few heard at both ends is "cannot say", not "nothing changed". */
    observore_surroundings_reset();
    observore_surroundings_note(1, -50, SECS(0));
    observore_surroundings_mark(SECS(1), SECS(300));
    observore_surroundings_note(1, -80, SECS(60));
    CHECK(observore_surroundings_faded_db() == INT_MIN,
          "one access point cannot answer it, got %d",
          observore_surroundings_faded_db());

    /* Stale entries must not make a new place look familiar: an access point
     * last heard an hour ago is not part of "here". */
    observore_surroundings_reset();
    observore_surroundings_note(7, -50, SECS(0));
    observore_surroundings_mark(SECS(4000), SECS(300));
    CHECK(observore_surroundings_marked() == 0,
          "an hour-old access point is not part of here, got %u",
          (unsigned)observore_surroundings_marked());
    CHECK(observore_surroundings_overlap_pct() == -1,
          "and with nothing marked the answer is 'cannot say', got %d",
          observore_surroundings_overlap_pct());

    observore_surroundings_reset();
}

static void test_a_crowd_cannot_hide_a_finding(void)
{
    banner("the screen lists the heaviest first, not merely the newest");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* A body camera, seen once and then quiet. */
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x21, 0x22, 0x23};
    uint8_t plain[] = {0x02, 0x01, 0x06};
    observore_observation_t cam = {.mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .adv = plain, .adv_len = sizeof(plain)};
    observore_track_observe(&cam, SECS(0));

    /* Then a roomful of newer, lighter things. Twelve rows on the glass and
     * more devices than that in the table: with recency alone the camera
     * falls off the bottom, which is how a crowd hides the one finding that
     * matters. */
    for (int i = 0; i < 14; i++) {
        uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, (uint8_t)(0x60 + i), 0xFE};
        uint8_t mac[6] = {0x70, 0x11, 0x22, 0x33, 0x44, (uint8_t)i};
        observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                     .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
        for (int t = 100; t <= 410; t += 100) {
            observore_track_observe(&o, SECS(t));
        }
    }

    observore_event_t snap[12];
    size_t n = observore_track_snapshot(snap, 12);
    CHECK(n == 12, "the screen asks for twelve, got %u", (unsigned)n);
    CHECK(snap[0].cls == OBSERVORE_CLASS_BODYCAM,
          "the body camera is still first, an hour later and quiet (got %s)",
          observore_class_desc(snap[0].cls)->name);

    /* And a follower must not outrank identified equipment, which it did on
     * the first attempt: the class carries four points in the table while
     * contributing at most the cap to a score, so sorting on the raw number
     * put a screenful of unidentified phones above a Flipper. Seen on
     * hardware after a trip -- nine devices, and the list showed followers. */
    observore_track_init();
    uint8_t flip[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x29, 0x0E, 0x00, 0x01};
    const uint8_t fmac[6] = {0x80, 0xE1, 0x27, 0x44, 0x55, 0x66};
    observore_observation_t hunter = {.mac = fmac, .src = OBSERVORE_SRC_BLE,
                                      .rssi = -60, .adv = flip,
                                      .adv_len = sizeof(flip)};
    observore_track_observe(&hunter, SECS(0));
    for (int i = 0; i < 4; i++) {
        uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, (uint8_t)(0x80 + i), 0xFE};
        uint8_t mac[6] = {0x90, 0x11, 0x22, 0x33, 0x44, (uint8_t)i};
        observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                     .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
        for (int t = 100; t <= 410; t += 100) {
            observore_track_observe(&o, SECS(t));
        }
    }
    n = observore_track_snapshot(snap, 12);
    CHECK(n >= 5 && snap[0].cls == OBSERVORE_CLASS_HUNTER,
          "the Flipper is listed above four fresher followers (got %s)",
          n ? observore_class_desc(snap[0].cls)->name : "nothing");

    observore_track_init();
}

static void test_something_that_came_with_you(void)
{
    banner("a follower that crosses a journey is tailing, and alerts");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();
    observore_track_set_journeys(0);

    uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x31, 0xFE};
    const uint8_t mac[6] = {0x50, 0x01, 0x02, 0x03, 0x04, 0x05};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                 .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&o, SECS(t));
    }
    observore_event_t snap[8];
    size_t n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "it starts as an ordinary follower");

    observore_status_t st;
    observore_track_status(&st, SECS(320));
    CHECK(st.level == OBSERVORE_LEVEL_CLEAR,
          "which cannot move the verdict, journey or no journey (level %s)",
          observore_level_name(st.level));

    /* Announced as a follower, which is what it is at this point. This drain
     * is what the test was missing: without it the device had never been
     * reported, so the check below passed whether or not the promotion
     * re-announced anything. */
    observore_event_t drained[4];
    size_t d = observore_track_drain_new(drained, 4);
    CHECK(d == 1 && drained[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "reported once as a follower (drained %u)", (unsigned)d);
    CHECK(observore_track_drain_new(drained, 4) == 0, "and not twice");

    /* The board is carried somewhere -- and this device is still beside it
     * at full strength while it is away, which is what "came with you"
     * means. Presence before and after is not enough: a round trip returns
     * home, where everything that never moved is in range at both ends. */
    observore_track_set_at_far_end(true);
    for (int t = 320; t <= 380; t += 10) {
        observore_track_observe(&o, SECS(t));    /* heard throughout, -55 dBm */
    }
    observore_track_set_journeys(1);
    observore_track_set_at_far_end(false);

    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_TAILING,
          "after a journey it is tailing, not merely persistent");
    CHECK(strcmp(snap[0].label, "came with you") == 0,
          "and says so plainly (got \"%s\")", snap[0].label);

    observore_track_status(&st, SECS(400));
    CHECK(st.level == OBSERVORE_LEVEL_ALERT,
          "one device that came with you is an alert on its own (score %u)",
          st.score);

    d = observore_track_drain_new(drained, 4);
    CHECK(d == 1 && drained[0].cls == OBSERVORE_CLASS_TAILING,
          "the promotion is announced again, as tailing (drained %u)",
          (unsigned)d);
    CHECK(observore_track_drain_new(drained, 4) == 0,
          "and only once, not on every sighting afterwards");

    /* The house, which is what the first version of this promoted. One walk
     * to a garden turned twelve devices that had sat indoors for nine hours
     * into things that had followed somebody, because a round trip puts
     * everything at home in range at both ends. */
    observore_track_init();
    observore_track_set_at_far_end(false);
    observore_track_set_journeys(0);
    uint8_t h_adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x44, 0xFE};
    const uint8_t h_mac[6] = {0x52, 0x11, 0x22, 0x33, 0x44, 0x55};
    observore_observation_t home = {.mac = h_mac, .src = OBSERVORE_SRC_BLE,
                                    .rssi = -50, .addr_random = true,
                                    .adv = h_adv, .adv_len = sizeof(h_adv)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&home, SECS(t));
    }
    observore_track_set_at_far_end(true);
    /* Audible from the end of the garden, and much fainter for it. */
    observore_observation_t faint = home;
    faint.rssi = -78;
    for (int t = 320; t <= 380; t += 10) {
        observore_track_observe(&faint, SECS(t));
    }
    observore_track_set_journeys(1);
    observore_track_set_at_far_end(false);
    /* Back indoors, loud again -- which is exactly what it looked like
     * before, and must still not count. */
    observore_track_observe(&home, SECS(420));
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "a device left at home is still only a follower (got %s)",
          n ? observore_class_desc(snap[0].cls)->name : "nothing");

    /* And one not heard at all while away cannot claim to have come. */
    observore_track_init();
    observore_track_set_at_far_end(false);
    observore_track_set_journeys(0);
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&home, SECS(t));
    }
    observore_track_set_at_far_end(true);
    observore_track_set_journeys(1);     /* silence throughout the trip */
    observore_track_set_at_far_end(false);
    observore_track_observe(&home, SECS(420));
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "silence while away is not evidence of having come along (got %s)",
          n ? observore_class_desc(snap[0].cls)->name : "nothing");

    /* The one the second walk found: a device that is faint everywhere.
     * A neighbour's phone heard at -72 dBm from the house and -72 dBm from
     * the garden has nothing to fade, so a rule that only measures change
     * calls it a companion. */
    observore_track_init();
    observore_track_set_at_far_end(false);
    observore_track_set_journeys(0);
    uint8_t d_adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x45, 0xFE};
    const uint8_t d_mac[6] = {0x53, 0x01, 0x02, 0x03, 0x04, 0x05};
    observore_observation_t distant = {.mac = d_mac, .src = OBSERVORE_SRC_BLE,
                                       .rssi = -72, .addr_random = true,
                                       .adv = d_adv, .adv_len = sizeof(d_adv)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&distant, SECS(t));
    }
    observore_track_set_at_far_end(true);
    for (int t = 320; t <= 380; t += 10) {
        observore_track_observe(&distant, SECS(t));   /* unchanged, -72 */
    }
    observore_track_set_journeys(1);
    observore_track_set_at_far_end(false);
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "faint everywhere is not in your pocket (got %s)",
          n ? observore_class_desc(snap[0].cls)->name : "nothing");

    /* And one heard a couple of times in passing has not travelled with
     * anybody either. */
    observore_track_init();
    observore_track_set_at_far_end(false);
    observore_track_set_journeys(0);
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&o, SECS(t));
    }
    observore_track_set_at_far_end(true);
    observore_track_observe(&o, SECS(330));
    observore_track_observe(&o, SECS(340));
    observore_track_set_journeys(1);
    observore_track_set_at_far_end(false);
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "twice in passing is not all the way (got %s)",
          n ? observore_class_desc(snap[0].cls)->name : "nothing");

    observore_track_init();
    observore_track_set_journeys(0);
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&o, SECS(t));
    }

    /* The failure that would make this useless: everything in the room is
     * not suddenly following you because the room moved. A device first
     * heard after the journey has crossed nothing. */
    uint8_t adv2[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x32, 0xFE};
    const uint8_t mac2[6] = {0x51, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
    observore_observation_t o2 = {.mac = mac2, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                  .addr_random = true, .adv = adv2, .adv_len = sizeof(adv2)};
    for (int t = 400; t <= 710; t += 100) {
        observore_track_observe(&o2, SECS(t));
    }
    n = observore_track_snapshot(snap, 8);
    for (size_t i = 0; i < n; i++) {
        if (memcmp(snap[i].mac, mac2, 6) == 0) {
            CHECK(snap[i].cls == OBSERVORE_CLASS_FOLLOWER,
                  "a device met after the journey is only a follower (got %s)",
                  observore_class_desc(snap[i].cls)->name);
        }
    }

    /* And a board that cannot feel a journey never promotes anything, which
     * is every board but one. */
    observore_track_init();
    observore_track_set_journeys(0);
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&o, SECS(t));
    }
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].cls == OBSERVORE_CLASS_FOLLOWER,
          "with no motion sensor, nothing is ever tailing");

    observore_track_set_journeys(0);
}

static void test_a_crowd_is_not_an_emergency(void)
{
    banner("a room full of people must not raise an alert");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* Twelve strangers, each with a phone, each in the room for the evening.
     * Distinct advert shapes so nothing is folded together by rotation
     * continuity. */
    for (int i = 0; i < 12; i++) {
        uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, (uint8_t)(0x20 + i), 0xFE};
        uint8_t mac[6] = {0x40, 0x11, 0x22, 0x33, 0x44, (uint8_t)i};
        observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                     .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
        for (int t = 0; t <= 310; t += 100) {
            observore_track_observe(&o, SECS(t));
        }
    }
    observore_status_t st;
    observore_track_status(&st, SECS(320));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 12, "all twelve are listed");
    CHECK(st.score <= OBSERVORE_FOLLOWER_SCORE_CAP,
          "but presence is capped at %u, got %u",
          OBSERVORE_FOLLOWER_SCORE_CAP, st.score);
    CHECK(st.level == OBSERVORE_LEVEL_CLEAR,
          "so a crowd reads clear, with all twelve listed (level %s)",
          observore_level_name(st.level));

    /* Rotation does not lift the ceiling either, which took hardware to
     * learn: every modern phone rotates its Bluetooth address every quarter
     * of an hour, so "survived a rotation" describes a phone behaving
     * normally rather than a device evading notice. A house full of them put
     * a board at seventeen and alert. */
    observore_track_init();
    for (int i = 0; i < 5; i++) {
        uint8_t a1[6] = {0x7A, 0x01, 0x02, 0x03, 0x04, (uint8_t)i};
        uint8_t a2[6] = {0x7B, 0x06, 0x07, 0x08, 0x09, (uint8_t)i};
        uint8_t shape[] = {0x02, 0x01, 0x06, 0x03, 0x03, (uint8_t)(0x40 + i), 0xFE};
        observore_observation_t r1 = {.mac = a1, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                      .addr_random = true, .adv = shape, .adv_len = sizeof(shape)};
        observore_observation_t r2 = r1; r2.mac = a2;
        for (int t = 0; t <= 310; t += 100) {
            observore_track_observe(&r1, SECS(t));
        }
        observore_track_observe(&r2, SECS(330));   /* quiet 20s: a rotation */
    }
    observore_track_status(&st, SECS(340));
    CHECK(st.score <= OBSERVORE_FOLLOWER_SCORE_CAP,
          "five rotated followers are still capped at %u, got %u",
          OBSERVORE_FOLLOWER_SCORE_CAP, st.score);
    CHECK(st.level == OBSERVORE_LEVEL_CLEAR,
          "and cannot move the verdict at all (level %s)",
          observore_level_name(st.level));

    /* The invariant the ceiling buys: unidentified devices cannot carry an
     * identified one over the line either. A Flipper in an empty room and a
     * Flipper in a crowd read the same, because the crowd is not evidence
     * about the Flipper. */
    uint8_t flip[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x29, 0x0E, 0x00, 0x01};
    const uint8_t flipmac[6] = {0x80, 0xE1, 0x27, 0x01, 0x02, 0x03};
    observore_observation_t f = {.mac = flipmac, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                 .adv = flip, .adv_len = sizeof(flip)};
    observore_track_observe(&f, SECS(341));
    observore_track_status(&st, SECS(341));
    CHECK(st.level == OBSERVORE_LEVEL_CAUTION,
          "a hunter among five followers is caution, not alert (score %u)", st.score);

    /* And a single body camera, with nothing else in the room at all, is. */
    observore_track_init();
    const uint8_t axon1[6] = {0x00, 0x25, 0xDF, 0x11, 0x12, 0x13};
    uint8_t bare[] = {0x02, 0x01, 0x06};
    observore_observation_t solo = {.mac = axon1, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                    .adv = bare, .adv_len = sizeof(bare)};
    observore_track_observe(&solo, SECS(0));
    observore_track_status(&st, SECS(0));
    CHECK(st.level == OBSERVORE_LEVEL_ALERT,
          "one body camera, alone, is an alert (score %u)", st.score);

    /* What an alert is for: something identified as what it is. */
    observore_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x09, 0x08, 0x07};
    uint8_t plain[] = {0x02, 0x01, 0x06};
    observore_observation_t cam = {.mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .adv = plain, .adv_len = sizeof(plain)};
    observore_track_observe(&cam, SECS(0));
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E; findmy[1] = 0xFF; findmy[2] = 0x4C; findmy[3] = 0x00;
    findmy[4] = 0x12; findmy[5] = 0x19; findmy[6] = 0x10;
    const uint8_t tag[6] = {0x4A, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B};
    observore_observation_t trk = {.mac = tag, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .addr_random = true, .adv = findmy, .adv_len = sizeof(findmy)};
    observore_track_observe(&trk, SECS(1));
    observore_track_status(&st, SECS(1));
    CHECK(st.level == OBSERVORE_LEVEL_ALERT,
          "a body camera and a tracker together is what alert is for (score %u)",
          st.score);
}

static void test_scoring(void)
{
    banner("scoring");

    observore_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    observore_observation_t obs = {
        .mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -40,
        .adv = boring, .adv_len = sizeof(boring),
    };

    CHECK(observore_track_observe(&obs, SECS(0)), "bodycam not reported");
    observore_status_t st;
    observore_track_status(&st, SECS(0));
    CHECK(st.score == 6, "bodycam should score 6, got %u", st.score);
    /* One body camera, nothing else in the room, and the verdict is alert.
     * It was caution until the weights were fixed, which made the clearest
     * detection this device can make arrive as a shrug. */
    CHECK(st.level == OBSERVORE_LEVEL_ALERT, "a body camera alerts on its own");

    /* A chatty beacon cannot run the score away, and no longer needs a
     * cooldown to stop it: the score is what is present, and one device
     * present fifty times is still one device. */
    for (int i = 1; i < 50; i++) {
        observore_track_observe(&obs, SECS(i));
    }
    observore_track_status(&st, SECS(50));
    CHECK(st.score == 6, "fifty sightings of one device still score 6, got %u", st.score);

    /* Nor does time raise it. Under the old accumulator this reached seven by
     * t=200 and the ceiling within the hour, which is how "alert" became the
     * resting state of any room with something in it. */
    observore_track_observe(&obs, SECS(200));
    observore_track_status(&st, SECS(200));
    CHECK(st.score == 6, "the same device an hour later still scores 6, got %u", st.score);
    CHECK(st.level == OBSERVORE_LEVEL_ALERT, "and is still one device, not two");

    /* A second device of another class adds its own weight -- this is the
     * axis the score is supposed to measure. A Find My tracker, identified by
     * its payload rather than by a vendor prefix invented for a test. */
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E; findmy[1] = 0xFF; findmy[2] = 0x4C; findmy[3] = 0x00;
    findmy[4] = 0x12; findmy[5] = 0x19; findmy[6] = 0x10;
    const uint8_t tagmac[6] = {0x4A, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
    observore_observation_t o2 = {.mac = tagmac, .src = OBSERVORE_SRC_BLE,
                                  .rssi = -50, .addr_random = true,
                                  .adv = findmy, .adv_len = sizeof(findmy)};
    observore_track_observe(&o2, SECS(210));
    observore_track_status(&st, SECS(210));
    CHECK(st.score > 6, "a second device raises the score (got %u)", st.score);
    CHECK(st.level == OBSERVORE_LEVEL_ALERT, "two pieces of serious kit is still an alert");

    /* And it comes down when the room empties, which the accumulator could
     * not do while anything remained. The table forgets a device thirty
     * minutes after it was last heard. */
    observore_track_tick(SECS(210 + 31 * 60));
    observore_track_status(&st, SECS(210 + 31 * 60));
    CHECK(st.score == 0, "an empty room scores nothing, got %u", st.score);
    CHECK(st.level == OBSERVORE_LEVEL_CLEAR, "and reads clear again");

    /* The reading never depended on tick() having run, and still must not. */
    observore_track_init();
    observore_track_observe(&obs, SECS(0));
    observore_track_status(&st, SECS(120));
    CHECK(st.score == 6, "without tick(): expected 6, got %u", st.score);
}

static void test_rssi_floor(void)
{
    banner("rssi floor");

    observore_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    observore_observation_t obs = {
        .mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -99,
        .adv = boring, .adv_len = sizeof(boring),
    };
    CHECK(!observore_track_observe(&obs, SECS(0)), "-99 dBm must be dropped");

    observore_status_t st;
    observore_track_status(&st, SECS(0));
    CHECK(st.score == 0, "a dropped sighting must not score");
}

static void test_table_pressure(void)
{
    banner("device table eviction");

    observore_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    /* One confirmed bodycam, then far more unclassified noise than the table
     * can hold.  The bodycam must survive -- evicting it to make room for a
     * passing phone is the failure this guards against. */
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    observore_observation_t cam = {
        .mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -40,
        .adv = boring, .adv_len = sizeof(boring),
    };
    observore_track_observe(&cam, SECS(0));

    for (int i = 0; i < OBSERVORE_MAX_DEVICES * 2; i++) {
        uint8_t noise[6] = {0x4A, 0x00, 0x00,
                            (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i};
        observore_observation_t obs = {
            .mac = noise, .src = OBSERVORE_SRC_BLE, .rssi = -70,
            .adv = boring, .adv_len = sizeof(boring),
        };
        observore_track_observe(&obs, SECS(1 + i));
    }

    observore_event_t snap[OBSERVORE_MAX_DEVICES];
    size_t n = observore_track_snapshot(snap, OBSERVORE_MAX_DEVICES);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (memcmp(snap[i].mac, axon, 6) == 0) {
            found = true;
        }
    }
    CHECK(found, "classified device was evicted by unclassified noise");
}

static void test_snapshot_order(void)
{
    banner("snapshot ordering");

    observore_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};
    const uint8_t older[6] = {0x00, 0x25, 0xDF, 0x00, 0x00, 0x01};  /* Axon */
    const uint8_t newer[6] = {0xB4, 0x1E, 0x52, 0x00, 0x00, 0x02};  /* Flock */

    observore_observation_t a = {.mac = older, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .adv = boring, .adv_len = sizeof(boring)};
    observore_observation_t b = {.mac = newer, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .adv = boring, .adv_len = sizeof(boring)};
    observore_track_observe(&a, SECS(10));
    observore_track_observe(&b, SECS(20));

    observore_event_t snap[8];
    size_t n = observore_track_snapshot(snap, 8);
    CHECK(n == 2, "expected 2 devices, got %zu", n);
    CHECK(n == 2 && memcmp(snap[0].mac, newer, 6) == 0,
          "snapshot must be newest first");
}

static void test_wifi_remote_id(void)
{
    banner("wi-fi remote id");

    /* A drone flying on a generic Wi-Fi module: the OUI says nothing, so the
     * Remote ID element has to be what carries the detection. */
    const uint8_t generic[6] = {0x00, 0x00, 0x00, 0x11, 0x22, 0x33};
    observore_observation_t obs = {
        .mac = generic, .src = OBSERVORE_SRC_WIFI_SNIFF, .rssi = -55,
        .channel = 6, .ssid = "DroneOps", .remote_id = true,
    };
    observore_event_t ev;
    CHECK(observore_classify(&obs, &ev), "remote id frame not classified");
    CHECK(ev.cls == OBSERVORE_CLASS_DRONE, "should be a drone");
    CHECK(strcmp(ev.detail, "DroneOps") == 0, "ssid not carried, got '%s'",
          ev.detail);

    /* Remote ID outranks a vendor prefix that would otherwise win. */
    const uint8_t ring[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03};
    obs.mac = ring;
    CHECK(observore_classify(&obs, &ev), "no match");
    CHECK(ev.cls == OBSERVORE_CLASS_DRONE, "remote id must outrank the OUI");

    /* Without the flag it falls back to the ordinary path. */
    obs.remote_id = false;
    CHECK(observore_classify(&obs, &ev), "ring OUI should still match");
    CHECK(ev.cls == OBSERVORE_CLASS_CAMERA, "should fall back to camera");
}

static void test_mute(void)
{
    banner("mute rules");

    observore_mute_init();
    observore_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    const uint8_t ring[6]  = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03}; /* camera  */
    const uint8_t ring2[6] = {0x54, 0xE0, 0x19, 0x09, 0x09, 0x09}; /* same OUI */
    const uint8_t axon[6]  = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03}; /* bodycam */

    observore_observation_t o_ring  = {.mac = ring,  .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};
    observore_observation_t o_ring2 = {.mac = ring2, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};
    observore_observation_t o_axon  = {.mac = axon,  .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};

    /* Baseline: both report. */
    CHECK(observore_track_observe(&o_ring, SECS(0)), "ring should report");
    CHECK(observore_track_observe(&o_axon, SECS(0)), "axon should report");

    /* Mute one exact address.  Its neighbour on the same OUI must survive. */
    observore_track_init();
    observore_mute_rule_t r = {.kind = OBSERVORE_MUTE_MAC};
    memcpy(r.mac, ring, 6);
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "add mac rule");
    CHECK(!observore_track_observe(&o_ring, SECS(0)), "muted mac must be suppressed");
    CHECK(observore_track_observe(&o_ring2, SECS(0)), "a different mac must survive");

    observore_status_t st;
    observore_track_status(&st, SECS(0));
    CHECK(st.score == observore_class_points(OBSERVORE_CLASS_CAMERA),
          "only the unmuted device should have scored, got %u", st.score);

    /* Muting the vendor prefix takes both. */
    observore_mute_clear();
    observore_track_init();
    r = (observore_mute_rule_t){.kind = OBSERVORE_MUTE_OUI};
    memcpy(r.mac, ring, 3);
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "add oui rule");
    CHECK(!observore_track_observe(&o_ring, SECS(0)), "oui rule should suppress");
    CHECK(!observore_track_observe(&o_ring2, SECS(0)), "oui rule should suppress");
    CHECK(observore_track_observe(&o_axon, SECS(0)), "a different vendor must survive");

    /* Muting a class takes the class and nothing else. */
    observore_mute_clear();
    observore_track_init();
    r = (observore_mute_rule_t){.kind = OBSERVORE_MUTE_CLASS, .cls = OBSERVORE_CLASS_CAMERA};
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "add class rule");
    CHECK(!observore_track_observe(&o_ring, SECS(0)), "camera class muted");
    CHECK(observore_track_observe(&o_axon, SECS(0)), "bodycam must still report");

    /* A class rule must not swallow unclassified traffic, or muting cameras
     * would quietly disable the follower heuristic too. */
    const uint8_t stranger[6] = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55};
    observore_observation_t o_unknown = {.mac = stranger, .src = OBSERVORE_SRC_BLE,
                                     .rssi = -60, .addr_random = true,
                                     .adv = boring, .adv_len = sizeof(boring)};
    observore_track_observe(&o_unknown, SECS(0));
    observore_track_observe(&o_unknown, SECS(10));
    observore_track_observe(&o_unknown, SECS(20));
    CHECK(observore_track_observe(&o_unknown, SECS(400)),
          "follower heuristic must survive a class mute");

    /* SSID substring, case-insensitively. */
    observore_mute_clear();
    observore_track_init();
    r = (observore_mute_rule_t){.kind = OBSERVORE_MUTE_NAME};
    snprintf(r.ssid, sizeof(r.ssid), "lobby");
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "add ssid rule");
    const uint8_t ap[6] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x03};
    observore_observation_t o_ap = {.mac = ap, .src = OBSERVORE_SRC_WIFI_SCAN,
                                .rssi = -50, .ssid = "Lobby-CCTV-2"};
    CHECK(!observore_track_observe(&o_ap, SECS(0)), "ssid rule should suppress");
    o_ap.ssid = "Garage-CCTV-2";
    CHECK(observore_track_observe(&o_ap, SECS(0)), "a different ssid must survive");

    /* List hygiene. */
    observore_mute_clear();
    r = (observore_mute_rule_t){.kind = OBSERVORE_MUTE_CLASS, .cls = OBSERVORE_CLASS_CAMERA};
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "first add");
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "duplicate add should succeed");
    CHECK(observore_mute_count() == 1, "duplicates must not accumulate, got %zu",
          observore_mute_count());

    /* Rules that would match everything are refused. */
    observore_mute_rule_t empty_ssid = {.kind = OBSERVORE_MUTE_NAME};
    CHECK(observore_mute_add(&empty_ssid, NULL) == ESP_ERR_INVALID_ARG,
          "an empty ssid rule matches everything and must be refused");
    observore_mute_rule_t bad_class = {.kind = OBSERVORE_MUTE_CLASS,
                                   .cls = OBSERVORE_CLASS_UNKNOWN};
    CHECK(observore_mute_add(&bad_class, NULL) == ESP_ERR_INVALID_ARG,
          "muting the unknown class must be refused");

    CHECK(observore_mute_remove(99) == ESP_ERR_NOT_FOUND, "out of range remove");
    CHECK(observore_mute_remove(0) == ESP_OK, "in range remove");
    CHECK(observore_mute_count() == 0, "list should be empty");

    observore_mute_clear();
}

static void test_fingerprint(void)
{
    banner("advert fingerprinting");

    /* A Find My advert rotates its key on every address change.  The
     * fingerprint must survive that, or it is no better than the MAC. */
    uint8_t a[31] = {0};
    a[0] = 0x1E; a[1] = 0xFF; a[2] = 0x4C; a[3] = 0x00;
    a[4] = 0x12; a[5] = 0x19; a[6] = 0x10;
    for (int i = 7; i < 31; i++) {
        a[i] = (uint8_t)(i * 7);          /* today's key */
    }
    uint8_t b[31];
    memcpy(b, a, sizeof(b));
    for (int i = 7; i < 31; i++) {
        b[i] = (uint8_t)(i * 13);         /* tomorrow's key */
    }

    uint32_t fa = observore_fingerprint(a, sizeof(a));
    uint32_t fb = observore_fingerprint(b, sizeof(b));
    CHECK(fa != 0, "fingerprint should not be zero");
    CHECK(fa == fb, "a rotated payload must not change the fingerprint");

    /* A different company ID is a different kind of device. */
    uint8_t c[31];
    memcpy(c, a, sizeof(c));
    c[2] = 0x75; c[3] = 0x00;             /* Samsung instead of Apple */
    CHECK(observore_fingerprint(c, sizeof(c)) != fa,
          "a different company ID must change the fingerprint");

    /* So is a different name. */
    uint8_t n1[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'T', 'V', '-', '1'};
    uint8_t n2[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'T', 'V', '-', '2'};
    CHECK(observore_fingerprint(n1, sizeof(n1)) != observore_fingerprint(n2, sizeof(n2)),
          "different names must fingerprint differently");

    /* Nothing to hash yields zero, which means "no fingerprint". */
    CHECK(observore_fingerprint(NULL, 0) == 0, "NULL advert");
    uint8_t empty[] = {0x00, 0x00};
    CHECK(observore_fingerprint(empty, sizeof(empty)) == 0, "padding-only advert");
}

static void test_fingerprint_safety(void)
{
    banner("fingerprint rules cannot silence a threat");

    observore_mute_init();
    observore_track_init();

    /* An AirTag-shaped advert on a rotating address. */
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E; findmy[1] = 0xFF; findmy[2] = 0x4C; findmy[3] = 0x00;
    findmy[4] = 0x12; findmy[5] = 0x19; findmy[6] = 0x10;

    const uint8_t mine[6]  = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55};
    observore_observation_t obs = {.mac = mine, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                               .addr_random = true,
                               .adv = findmy, .adv_len = sizeof(findmy)};

    observore_mute_rule_t fp = {.kind = OBSERVORE_MUTE_FINGERPRINT,
                            .fingerprint = observore_fingerprint(findmy,
                                                             sizeof(findmy))};
    CHECK(observore_mute_add(&fp, NULL) == ESP_OK, "fingerprint rule should be accepted");

    /* Muting your own tracker by fingerprint would silence a stranger's too,
     * so the rule must not apply to a tracker at all. */
    CHECK(observore_track_observe(&obs, SECS(0)),
          "a tracker must still report despite a matching fingerprint rule");
    observore_status_t st;
    observore_track_status(&st, SECS(0));
    CHECK(st.class_counts[OBSERVORE_CLASS_TRACKER] == 1, "tracker should be counted");

    /* The same rule does work on an unprotected class. */
    CHECK(observore_mute_class_is_protected(OBSERVORE_CLASS_TRACKER), "tracker protected");
    CHECK(observore_mute_class_is_protected(OBSERVORE_CLASS_BODYCAM), "bodycam protected");
    CHECK(observore_mute_class_is_protected(OBSERVORE_CLASS_FOLLOWER),
          "follower protected -- it is the anti-stalking case");
    CHECK(!observore_mute_class_is_protected(OBSERVORE_CLASS_CAMERA),
          "cameras are street furniture and may be muted wholesale");

    observore_mute_clear();
    observore_track_init();
    uint8_t plain[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xAA, 0xBB};
    observore_observation_t un = {.mac = mine, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                              .addr_random = true,
                              .adv = plain, .adv_len = sizeof(plain)};
    observore_mute_rule_t fp2 = {.kind = OBSERVORE_MUTE_FINGERPRINT,
                             .fingerprint = observore_fingerprint(plain,
                                                              sizeof(plain))};
    CHECK(observore_mute_add(&fp2, NULL) == ESP_OK, "add");

    /* Unclassified traffic IS suppressed, including across a MAC change --
     * that is the whole point of fingerprinting. */
    observore_track_observe(&un, SECS(0));
    const uint8_t rotated[6] = {0x4A, 0x99, 0x88, 0x77, 0x66, 0x55};
    observore_observation_t un2 = un;
    un2.mac = rotated;
    observore_track_observe(&un2, SECS(10));
    observore_event_t nearby[8];
    CHECK(observore_track_nearby(nearby, 8) == 0,
          "a fingerprint rule must suppress the device under any address");

    /* A zero fingerprint rule would match every nameless advert. */
    observore_mute_rule_t zero = {.kind = OBSERVORE_MUTE_FINGERPRINT, .fingerprint = 0};
    CHECK(observore_mute_add(&zero, NULL) == ESP_ERR_INVALID_ARG,
          "a zero fingerprint rule must be refused");
}

static void test_ignoring_takes_it_off_the_screen(void)
{
    banner("ignoring a device removes it now, not in half an hour");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* A classified device, because the table's count is of classified rows --
     * and a fixture is exactly the case that prompted this. */
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x06, 0x09, 'E', 'n', 'v', 'o', 'y'};
    const uint8_t mac[6] = {0x94, 0x34, 0x69, 0x01, 0x02, 0x03};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                 .addr_random = false, .adv = adv, .adv_len = sizeof(adv)};
    for (int t = 0; t <= 400; t += 100) {
        observore_track_observe(&o, SECS(t));
    }
    observore_status_t st;
    observore_track_status(&st, SECS(400));
    CHECK(st.device_count == 1, "it is in the table to begin with");

    /* Ignore it the way a tap on the screen does. */
    observore_mute_rule_t rule = {.kind = OBSERVORE_MUTE_MAC};
    memcpy(rule.mac, mac, 6);
    CHECK(observore_mute_add(&rule, NULL) == ESP_OK, "the rule is written");
    CHECK(observore_track_forget_muted() == 1, "and the row goes with it");
    observore_track_status(&st, SECS(400));
    CHECK(st.device_count == 0,
          "so the screen stops showing what was just dismissed");

    /* The sweep must not charge those rows to the rule: a rule that swept a
     * crowded table could otherwise retire itself for covering a population
     * it never actually silenced. */
    observore_mute_stat_t stat;
    CHECK(observore_mute_stat(0, &stat), "the rule has statistics");
    CHECK(stat.suppressed == 0,
          "and the sweep did not inflate them (got %u)", (unsigned)stat.suppressed);

    observore_mute_clear();
}

static void test_fast_pair_split(void)
{
    banner("earbuds pairing are not a tracker following you");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* The discoverable frame: exactly three bytes of service data, a 24-bit
     * model ID. This is a device in pairing mode. */
    uint8_t pairing[] = {0x02, 0x01, 0x06,
                         0x06, 0x16, 0x2C, 0xFE, 0x0E, 0xA0, 0x11};
    const uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                 .adv = pairing, .adv_len = sizeof(pairing)};
    observore_event_t ev;
    CHECK(observore_classify(&o, &ev), "a pairing beacon is still reported");
    CHECK(ev.cls == OBSERVORE_CLASS_ACCESSORY, "as an accessory, not a tracker");
    CHECK(observore_class_points(OBSERVORE_CLASS_ACCESSORY) <
          observore_class_points(OBSERVORE_CLASS_TRACKER),
          "and scores below one");

    /* It must still be visible -- the whole point is that nothing is dropped,
     * only weighted differently. */
    CHECK(observore_track_observe(&o, SECS(0)), "and it reaches the table");

    /* Anything else under the same UUID keeps full tracker weight, because no
     * byte here reliably tells a tag from a headphone and the expensive
     * mistake is the other direction. */
    observore_track_init();
    uint8_t tag[] = {0x02, 0x01, 0x06,
                     0x0B, 0x16, 0x2C, 0xFE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    observore_observation_t ot = o; ot.adv = tag; ot.adv_len = sizeof(tag);
    observore_event_t ev2;
    CHECK(observore_classify(&ot, &ev2), "a longer Fast Pair frame classifies");
    CHECK(ev2.cls == OBSERVORE_CLASS_TRACKER, "as a tracker, at full weight");

    observore_mute_clear();
}

static void test_fixtures_are_not_followers(void)
{
    banner("a named appliance is not something following you");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* An Enphase Envoy: a public address, a stable name, bolted to a wall. */
    uint8_t adv[] = {0x02, 0x01, 0x06,
                     0x0D, 0x09, 'E', 'n', 'v', 'o', 'y', ' ', '1', '2', '1', '9', '0', '1'};
    const uint8_t mac[6] = {0x94, 0x34, 0x69, 0x9D, 0x28, 0xC0};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -88,
                                 .addr_random = false, .adv = adv, .adv_len = sizeof(adv)};
    observore_event_t ev;
    CHECK(observore_classify(&o, &ev), "it classifies from its name");
    CHECK(ev.cls == OBSERVORE_CLASS_FIXTURE, "as a fixture, not a follower");
    CHECK(observore_class_points(OBSERVORE_CLASS_FIXTURE) == 0, "worth no points");

    /* And a nameless appliance with a fixed address -- no keyword to match --
     * still must not be promoted at five minutes the way a random address is.
     * It gets an hour, because an hour beside you is worth saying either way. */
    observore_track_init();
    uint8_t bare[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'P', 'r', 'n', 't'};
    const uint8_t pm[6] = {0x3C, 0x2A, 0xF4, 0x01, 0x02, 0x03};
    observore_observation_t op = {.mac = pm, .src = OBSERVORE_SRC_BLE, .rssi = -70,
                                 .addr_random = false, .adv = bare, .adv_len = sizeof(bare)};
    for (int t = 0; t <= 400; t += 100) {
        observore_track_observe(&op, SECS(t));
    }
    observore_status_t st;
    observore_track_status(&st, SECS(400));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 0,
          "a named device on a fixed address is not a follower at seven minutes");

    /* An hour of it is a different statement. */
    observore_track_observe(&op, SECS(3700));
    observore_track_status(&st, SECS(3700));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 1,
          "but an hour beside you still counts");

    /* The rotating, nameless case is untouched: five minutes is still five
     * minutes for something trying not to be identified. */
    observore_track_init();
    uint8_t quiet[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    const uint8_t rm[6] = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55};
    observore_observation_t oq = {.mac = rm, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                 .addr_random = true, .adv = quiet, .adv_len = sizeof(quiet)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&oq, SECS(t));
    }
    observore_track_status(&st, SECS(310));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 1,
          "a nameless rotating address is still promoted at five minutes");

    observore_mute_clear();
}

static void test_hunter_gear(void)
{
    banner("gear that transmits at other radios");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* Flipper Zero by company ID. 0x0E29 is Flipper Devices in the SIG list;
     * the widespread 0x0FBA is Cosonic, who make headsets, and every project
     * that copied Marauder's constant flags their customers as hacking
     * tools. */
    uint8_t flip[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x29, 0x0E, 0x01, 0x02};
    const uint8_t mac[6] = {0x0C, 0xFA, 0x22, 0x01, 0x02, 0x03};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                 .adv = flip, .adv_len = sizeof(flip)};
    observore_event_t ev;
    CHECK(observore_classify(&o, &ev), "a Flipper advert classifies");
    CHECK(ev.cls == OBSERVORE_CLASS_HUNTER, "as hunter gear");
    CHECK(strcmp(ev.label, "Flipper Zero") == 0, "named (got \"%s\")", ev.label);

    /* The company ID everybody copied must NOT be treated as a Flipper. */
    uint8_t cosonic[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0xBA, 0x0F, 0x01, 0x02};
    observore_observation_t oc = o; oc.adv = cosonic; oc.adv_len = sizeof(cosonic);
    observore_event_t ev2;
    bool got = observore_classify(&oc, &ev2);
    CHECK(!got || ev2.cls != OBSERVORE_CLASS_HUNTER,
          "a headset maker's company ID is not a Flipper");

    /* And by service UUID, one per hardware colour. */
    uint8_t uuid[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x82, 0x30};
    observore_observation_t ou = o; ou.adv = uuid; ou.adv_len = sizeof(uuid);
    observore_event_t ev3;
    CHECK(observore_classify(&ou, &ev3) && ev3.cls == OBSERVORE_CLASS_HUNTER,
          "a Flipper service UUID classifies too");

    /* A pwnagotchi volunteers the marker in its own beacon. */
    observore_observation_t op = {.mac = mac, .src = OBSERVORE_SRC_WIFI_SNIFF,
                                  .rssi = -60, .pwnagotchi = true, .ssid = "throwaway"};
    observore_event_t ev4;
    CHECK(observore_classify(&op, &ev4), "a pwnagotchi beacon classifies");
    CHECK(ev4.cls == OBSERVORE_CLASS_HUNTER && strcmp(ev4.label, "pwnagotchi") == 0,
          "as hunter gear, named (got \"%s\")", ev4.label);

    /* A Pineapple's management AP, and not somebody's fruit-themed network. */
    observore_observation_t opi = {.mac = mac, .src = OBSERVORE_SRC_WIFI_SCAN,
                                   .rssi = -60, .ssid = "Pineapple_A1B2"};
    observore_event_t ev5;
    CHECK(observore_classify(&opi, &ev5) && ev5.cls == OBSERVORE_CLASS_HUNTER,
          "Pineapple_XXXX is the documented default");
    observore_observation_t okitchen = opi; okitchen.ssid = "Pineapple Villa";
    observore_event_t ev6;
    got = observore_classify(&okitchen, &ev6);
    CHECK(!got || ev6.cls != OBSERVORE_CLASS_HUNTER,
          "but a home network merely called Pineapple is not one");

    /* A flood is an act rather than a device, and outranks any label. */
    observore_observation_t od = {.mac = mac, .src = OBSERVORE_SRC_WIFI_SNIFF,
                                  .rssi = -40, .deauth_flood = true};
    observore_event_t ev7;
    CHECK(observore_classify(&od, &ev7), "a deauth flood classifies");
    CHECK(ev7.cls == OBSERVORE_CLASS_DEAUTH, "as its own thing");
    CHECK(observore_class_points(OBSERVORE_CLASS_DEAUTH) >
          observore_class_points(OBSERVORE_CLASS_HUNTER),
          "and scores above the mere presence of a tool");

    observore_mute_clear();
}

static void test_squachmesh(void)
{
    banner("SquachMesh: another detector announcing itself");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* Company 0xFFFF, "SQM1", version 1, appearance word, reserved flags.
     * Read off their source, not guessed from the air. */
    uint8_t adv[] = {0x02, 0x01, 0x06,
                     0x0B, 0xFF, 0xFF, 0xFF, 'S', 'Q', 'M', '1', 0x01,
                     0x34, 0x12, 0x00};
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    observore_observation_t o = {.mac = mac, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                 .addr_random = false, .adv = adv, .adv_len = sizeof(adv)};
    observore_event_t ev;
    CHECK(observore_classify(&o, &ev), "a SquachMesh advert is classified");
    CHECK(ev.cls == OBSERVORE_CLASS_PEER_DETECTOR, "as a peer detector, not a threat");
    CHECK(strcmp(ev.label, "SquachWatch") == 0, "named plainly (got \"%s\")", ev.label);

    /* It must not move the score. Another detector in the room is a fact
     * about the room; a class that raised the level would make a meetup read
     * as an incident. */
    CHECK(observore_class_points(OBSERVORE_CLASS_PEER_DETECTOR) == 0,
          "and worth no points");

    /* The company ID alone means nothing -- it is the SIG's reserved
     * non-production ID, shared with every other hobby project. Same company,
     * different magic, must not be claimed as one of theirs. */
    uint8_t other[] = {0x02, 0x01, 0x06,
                       0x0B, 0xFF, 0xFF, 0xFF, 'N', 'O', 'P', 'E', 0x01,
                       0x34, 0x12, 0x00};
    observore_observation_t oo = o; oo.adv = other; oo.adv_len = sizeof(other);
    observore_event_t ev2;
    bool got = observore_classify(&oo, &ev2);
    CHECK(!got || ev2.cls != OBSERVORE_CLASS_PEER_DETECTOR,
          "somebody else's 0xFFFF payload is not a SquachWatch");

    /* Too short to carry the header is not a match either. */
    uint8_t stub[] = {0x02, 0x01, 0x06, 0x06, 0xFF, 0xFF, 0xFF, 'S', 'Q', 'M'};
    observore_observation_t os = o; os.adv = stub; os.adv_len = sizeof(stub);
    observore_event_t ev3;
    got = observore_classify(&os, &ev3);
    CHECK(!got || ev3.cls != OBSERVORE_CLASS_PEER_DETECTOR,
          "a truncated header is refused rather than half-read");

    /* A typed name is twelve bytes of somebody else's choosing, landing in a
     * string this device draws. It is repeated only while it stays printable. */
    uint8_t named[] = {0x02, 0x01, 0x06,
                       0x17, 0xFF, 0xFF, 0xFF, 'S', 'Q', 'M', '1', 0x01,
                       0x34, 0x12, 0x00,
                       'R', 'o', 'l', 'a', 'n', 'd', 0, 0, 0, 0, 0, 0};
    observore_observation_t on = o; on.adv = named; on.adv_len = sizeof(named);
    observore_event_t ev4;
    CHECK(observore_classify(&on, &ev4), "a named SquachWatch classifies");
    CHECK(strcmp(ev4.detail, "Roland") == 0, "carries the typed name (got \"%s\")", ev4.detail);
    CHECK(strcmp(ev4.label, "SquachWatch") == 0, "and still says what it is");

    uint8_t nasty[] = {0x02, 0x01, 0x06,
                       0x17, 0xFF, 0xFF, 0xFF, 'S', 'Q', 'M', '1', 0x01,
                       0x34, 0x12, 0x00,
                       'a', 0x1B, '[', '2', 'J', 0, 0, 0, 0, 0, 0, 0};
    observore_observation_t oz = o; oz.adv = nasty; oz.adv_len = sizeof(nasty);
    observore_event_t ev5;
    CHECK(observore_classify(&oz, &ev5), "a hostile name still classifies");
    CHECK(ev5.detail[0] == '\0',
          "but an escape sequence is not repeated to the screen (got \"%s\")",
          ev5.detail);
    CHECK(strcmp(ev5.label, "SquachWatch") == 0, "while the class is unaffected");

    observore_mute_clear();
}

static void test_a_baseline_may_not_silence_a_kind(void)
{
    banner("a baseline silences a protected device by address, never by name");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* A Flipper, named as every Flipper is: the word and then whatever the
     * owner called it. The name is sixteen characters, comfortably past the
     * length a baseline requires, which is exactly how the real one got a
     * name rule and went unreported on two boards for a week. */
    uint8_t flip[32] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x29, 0x0E, 0x00, 0x01};
    size_t n = 9;
    const char *name = "Flipper Arala75h";
    flip[n++] = (uint8_t)(1 + strlen(name));
    flip[n++] = 0x09;
    memcpy(&flip[n], name, strlen(name));
    n += strlen(name);
    const uint8_t fmac[6] = {0x80, 0xE1, 0x27, 0x8D, 0xAD, 0x84};
    observore_observation_t f = {.mac = fmac, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                 .adv = flip, .adv_len = n};
    CHECK(observore_track_observe(&f, SECS(0)), "the Flipper is reported to begin with");

    static observore_event_t scratch[32];
    observore_baseline_t r;
    observore_mute_baseline(scratch, 32, &r);

    CHECK(r.by_name == 0, "no name rule for a protected class (got %zu)", r.by_name);
    CHECK(r.by_mac == 1, "it is silenced by its address instead (got %zu)", r.by_mac);
    CHECK(r.protected_muted == 1,
          "and the baseline says it silenced one (got %zu)", r.protected_muted);
    CHECK(r.protected_example == OBSERVORE_CLASS_HUNTER,
          "naming the class it was (got %s)",
          observore_class_desc((observore_class_t)r.protected_example)->name);

    /* The owner's own is quiet. */
    CHECK(observore_mute_matches(fmac, OBSERVORE_CLASS_HUNTER, name, 0),
          "the one that was here is ignored");

    /* A stranger's, at another address, is not -- which is the whole point.
     * Under a name rule this was silenced too, because a name rule matches as
     * a substring and every Flipper's name begins with the same word. */
    const uint8_t other[6] = {0x80, 0xE1, 0x27, 0x01, 0x02, 0x03};
    CHECK(!observore_mute_matches(other, OBSERVORE_CLASS_HUNTER, "Flipper Zeta99x", 0),
          "another Flipper, elsewhere, is still reported");

    /* The same holds for the class this device most exists to find, where the
     * broadcast name is a model rather than a pet name -- so a name rule
     * would have quieted every one of that model anywhere. */
    observore_mute_clear();
    observore_track_init();
    uint8_t cam[32] = {0x02, 0x01, 0x06};
    size_t cn = 3;
    const char *model = "AXON BODY 3";
    cam[cn++] = (uint8_t)(1 + strlen(model));
    cam[cn++] = 0x09;
    memcpy(&cam[cn], model, strlen(model));
    cn += strlen(model);
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x0A, 0x0B, 0x0C};
    observore_observation_t c = {.mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                 .adv = cam, .adv_len = cn};
    observore_track_observe(&c, SECS(0));
    observore_mute_baseline(scratch, 32, &r);
    CHECK(r.by_name == 0, "no name rule for a body camera either (got %zu)", r.by_name);
    const uint8_t axon2[6] = {0x00, 0x25, 0xDF, 0x99, 0x98, 0x97};
    CHECK(!observore_mute_matches(axon2, OBSERVORE_CLASS_BODYCAM, model, 0),
          "a stranger's body camera of the same model is still reported");

    /* The follower class keeps the trade it was given deliberately: a
     * household phone earns that verdict by sitting still for five minutes,
     * and a MAC rule on a rotating address would be gone within the hour. */
    CHECK(!observore_mute_class_needs_address_rule(OBSERVORE_CLASS_FOLLOWER),
          "a follower may still be silenced by name");
    CHECK(observore_mute_class_needs_address_rule(OBSERVORE_CLASS_TRACKER) &&
              observore_mute_class_needs_address_rule(OBSERVORE_CLASS_BODYCAM) &&
              observore_mute_class_needs_address_rule(OBSERVORE_CLASS_HUNTER),
          "the classes worth finding may not");
    CHECK(!observore_mute_class_needs_address_rule(OBSERVORE_CLASS_CAMERA),
          "street furniture may be muted wholesale, which is the point of it");

    observore_mute_clear();
}

static void test_baseline_does_not_blind(void)
{
    banner("a baseline must not silence a whole population of devices");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* Three different phones broadcasting the same shape -- flags and one
     * service UUID, which is what a great many devices send and what makes a
     * fingerprint describe a model rather than a device. */
    uint8_t common[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    const uint8_t a[6] = {0x41, 0x11, 0x11, 0x11, 0x11, 0x11};
    const uint8_t b[6] = {0x42, 0x22, 0x22, 0x22, 0x22, 0x22};
    const uint8_t c[6] = {0x43, 0x33, 0x33, 0x33, 0x33, 0x33};
    observore_observation_t oa = {.mac = a, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                  .addr_random = true, .adv = common, .adv_len = sizeof(common)};
    observore_observation_t ob = oa; ob.mac = b;
    observore_observation_t oc = oa; oc.mac = c;
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&oa, SECS(t));
        observore_track_observe(&ob, SECS(t));
        observore_track_observe(&oc, SECS(t));
    }

    static observore_event_t scratch[32];
    observore_baseline_t r;
    observore_mute_baseline(scratch, 32, &r);
    CHECK(r.by_fingerprint == 0,
          "a shape shared by three devices is a model, not a device, so no "
          "fingerprint rule (got %zu)", r.by_fingerprint);
    CHECK(r.by_mac == 3, "each one is muted by its own address instead (got %zu)", r.by_mac);

    /* A fourth device of the same model, arriving afterwards, must still be
     * heard. This is the check the feature shipped without: the old test
     * proved a rule was written, not that the detector still worked.
     *
     * Asked of the mute layer directly, because track_observe() answers "is
     * this worth reporting", which a device seen once is not either way. */
    const uint8_t d[6] = {0x44, 0x44, 0x44, 0x44, 0x44, 0x44};
    uint32_t common_fp = observore_fingerprint(common, sizeof(common));
    CHECK(!observore_mute_matches(d, OBSERVORE_CLASS_UNKNOWN, NULL, common_fp),
          "a different device of the same model must still be heard");
    CHECK(observore_mute_matches(a, OBSERVORE_CLASS_UNKNOWN, NULL, common_fp),
          "while the three that were there are ignored by address");

    /* And the lone device keeps the durable rule it deserves. */
    observore_mute_clear();
    observore_track_init();
    uint8_t lone[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x9A, 0x00, 0x77, 0x77};
    observore_observation_t ol = {.mac = a, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                  .addr_random = true, .adv = lone, .adv_len = sizeof(lone)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&ol, SECS(t));
    }
    observore_mute_baseline(scratch, 32, &r);
    CHECK(r.by_fingerprint == 1,
          "a shape only one device carries still earns a fingerprint rule (got %zu)",
          r.by_fingerprint);

    /* A two-character broadcast name is a fragment, not an identity: as a
     * substring rule it silenced a hundred addresses on the bench. */
    observore_mute_clear();
    observore_track_init();
    uint8_t named[] = {0x02, 0x01, 0x06, 0x03, 0x09, 0x34, 0x32};   /* name "42" */
    observore_observation_t on = {.mac = b, .src = OBSERVORE_SRC_BLE, .rssi = -55,
                                  .addr_random = false, .adv = named, .adv_len = sizeof(named)};
    for (int t = 0; t <= 310; t += 100) {
        observore_track_observe(&on, SECS(t));
    }
    observore_mute_baseline(scratch, 32, &r);
    CHECK(r.by_name == 0, "a two-character name is too short to be a rule (got %zu)", r.by_name);
    CHECK(!observore_mute_matches((const uint8_t[]){1,2,3,4,5,6},
                                  OBSERVORE_CLASS_UNKNOWN, "PIXEL-4210", 0),
          "so something else whose name merely contains it is still heard");

    observore_mute_clear();
}

static void test_mute_rule_retires_when_it_covers_a_population(void)
{
    banner("a fingerprint rule that covers many addresses stops being honoured");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    uint8_t common[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    observore_mute_rule_t fp = {.kind = OBSERVORE_MUTE_FINGERPRINT,
                                .fingerprint = observore_fingerprint(common, sizeof(common))};
    CHECK(observore_mute_add(&fp, NULL) == ESP_OK, "add the fingerprint rule");

    /* Address after address, all the same shape -- a household of phones. */
    uint8_t mac[6] = {0x50, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t fpv = observore_fingerprint(common, sizeof(common));
    int suppressed = 0, heard = 0;
    for (int i = 0; i < 20; i++) {
        mac[5] = (uint8_t)i;
        if (observore_mute_matches(mac, OBSERVORE_CLASS_UNKNOWN, NULL, fpv)) {
            suppressed++;
        } else {
            heard++;
        }
    }
    CHECK(suppressed > 0, "it silences the first few, as asked");
    CHECK(heard > 0, "and stops once it is plainly describing a population "
                     "(suppressed %d, heard %d)", suppressed, heard);

    observore_mute_stat_t st;
    CHECK(observore_mute_stat(0, &st), "the rule reports what it has covered");
    CHECK(st.disabled, "and says it has been retired");
    CHECK(st.addresses > OBSERVORE_MUTE_ADDRESS_LIMIT, "having covered %u addresses",
          (unsigned)st.addresses);

    /* The verdict outlives the rule. Retirement is a fact about the shape, so
     * writing the same rule again does not resurrect it -- which is also what
     * makes it survivable across a reboot, where the counters do not. */
    observore_mute_rule_t again = fp;
    observore_mute_remove(0);
    CHECK(observore_mute_add(&again, NULL) == ESP_OK, "the same rule can be re-added");
    mac[5] = 0x99;
    CHECK(!observore_mute_matches(mac, OBSERVORE_CLASS_UNKNOWN, NULL, fpv),
          "but a shape already judged a population stays retired");

    /* Clearing the list is a fresh start, judgements included. */
    observore_mute_clear();
    CHECK(observore_mute_add(&again, NULL) == ESP_OK, "add it once more after a clear");
    CHECK(observore_mute_matches(mac, OBSERVORE_CLASS_UNKNOWN, NULL, fpv),
          "and it is honoured again");

    observore_mute_clear();
}

static void test_baseline_follower(void)
{
    banner("a baseline mutes a rotating follower by fingerprint, not by address");

    observore_mute_init();
    observore_mute_clear();
    observore_track_init();

    /* A plain phone-shaped advert on a rotating address, in the room long
     * enough to be promoted. */
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    const uint8_t a1[6] = {0x73, 0x85, 0x1A, 0x47, 0xA6, 0xAD};
    const uint8_t a2[6] = {0x64, 0x59, 0x0D, 0x93, 0xD7, 0xB7};
    observore_observation_t o1 = {.mac = a1, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                  .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
    observore_observation_t o2 = o1; o2.mac = a2;

    observore_track_observe(&o1, SECS(0));
    observore_track_observe(&o1, SECS(100));
    observore_track_observe(&o1, SECS(200));
    observore_track_observe(&o1, SECS(310));
    observore_status_t st;
    observore_track_status(&st, SECS(310));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 1, "promoted to follower first");

    /* The owner holds the button. */
    static observore_event_t scratch[OBSERVORE_MAX_DEVICES];
    observore_baseline_t r;
    observore_mute_baseline(scratch, OBSERVORE_MAX_DEVICES, &r);
    CHECK(r.added == 1, "one rule for the one device (got %zu)", r.added);
    CHECK(r.by_fingerprint == 1 && r.by_mac == 0,
          "a rotating follower gets a fingerprint rule, not a MAC that dies with the rotation");

    /* Tomorrow, under a fresh address, the same handset sits in the room
     * for as long as it likes and is never promoted again. */
    observore_track_init();
    observore_track_observe(&o2, SECS(90000));
    observore_track_observe(&o2, SECS(90100));
    observore_track_observe(&o2, SECS(90200));
    CHECK(!observore_track_observe(&o2, SECS(90310)),
          "the same advert shape under a new address must not report");
    observore_track_status(&st, SECS(90310));
    CHECK(st.class_counts[OBSERVORE_CLASS_FOLLOWER] == 0 && st.device_count == 0,
          "and never enters the table (followers %d, devices %d)",
          (int)st.class_counts[OBSERVORE_CLASS_FOLLOWER], (int)st.device_count);

    /* A follower on a public address keeps the more specific MAC rule. */
    observore_mute_clear();
    observore_track_init();
    const uint8_t pub[6] = {0x25, 0x10, 0x30, 0xB6, 0x39, 0x8E};
    observore_observation_t op = o1; op.mac = pub; op.addr_random = false;
    observore_track_observe(&op, SECS(0));
    observore_track_observe(&op, SECS(100));
    observore_track_observe(&op, SECS(200));
    observore_track_observe(&op, SECS(310));
    observore_mute_baseline(scratch, OBSERVORE_MAX_DEVICES, &r);
    CHECK(r.by_mac == 1 && r.by_fingerprint == 0,
          "a follower that keeps its address is muted by that address");

    /* A tracker is still never muted by fingerprint, baseline or not. */
    observore_mute_clear();
    observore_track_init();
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E; findmy[1] = 0xFF; findmy[2] = 0x4C; findmy[3] = 0x00;
    findmy[4] = 0x12; findmy[5] = 0x19; findmy[6] = 0x10;
    observore_observation_t ot = {.mac = a1, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                                  .addr_random = true, .adv = findmy, .adv_len = sizeof(findmy)};
    observore_track_observe(&ot, SECS(0));
    observore_mute_baseline(scratch, OBSERVORE_MAX_DEVICES, &r);
    CHECK(r.by_fingerprint == 0, "a tracker in a baseline is never a fingerprint rule");

    observore_mute_clear();
}

static void test_name_rule_matches_ble(void)
{
    banner("name rules across a MAC rotation");

    observore_mute_init();
    observore_track_init();

    /* A named BLE device that changes address must stay muted -- this is what
     * a MAC rule could never do. */
    uint8_t named[] = {0x02, 0x01, 0x06,
                       0x08, 0x09, 'E', 'n', 'c', 'h', 'a', 'r', 'g'};
    observore_mute_rule_t r = {.kind = OBSERVORE_MUTE_NAME};
    snprintf(r.ssid, sizeof(r.ssid), "Encharg");
    CHECK(observore_mute_add(&r, NULL) == ESP_OK, "add name rule");

    const uint8_t mac1[6] = {0x4A, 0x01, 0x02, 0x03, 0x04, 0x05};
    const uint8_t mac2[6] = {0x4A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    observore_observation_t o = {.mac = mac1, .src = OBSERVORE_SRC_BLE, .rssi = -50,
                             .addr_random = true,
                             .adv = named, .adv_len = sizeof(named)};
    CHECK(!observore_track_observe(&o, SECS(0)), "named device should be muted");
    o.mac = mac2;
    CHECK(!observore_track_observe(&o, SECS(60)),
          "still muted after the address rotates");

    observore_event_t nearby[8];
    CHECK(observore_track_nearby(nearby, 8) == 0,
          "neither address should be tracked");

    /* And it still works for Wi-Fi SSIDs, which is where it started. */
    observore_track_init();
    const uint8_t ap[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    observore_observation_t w = {.mac = ap, .src = OBSERVORE_SRC_WIFI_SCAN,
                             .rssi = -50, .ssid = "Encharger-Guest"};
    CHECK(!observore_track_observe(&w, SECS(0)), "ssid should match the name rule");
}

static void test_mac_parsing(void)
{
    banner("mac parsing");

    uint8_t mac[6];
    CHECK(observore_mute_parse_mac("AA:BB:CC:DD:EE:FF", mac, 6), "colon form");
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF, "wrong bytes");
    CHECK(observore_mute_parse_mac("aabbccddeeff", mac, 6), "bare form");
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF, "wrong bytes");
    CHECK(observore_mute_parse_mac("AA-BB-CC", mac, 3), "dash form, 3 bytes");

    /* A typo must be an error, not a rule that silently matches the wrong
     * device.  Trailing rubbish is the dangerous case. */
    CHECK(!observore_mute_parse_mac("AA:BB:CC:DD:EE:FF:00", mac, 6), "too long");
    CHECK(!observore_mute_parse_mac("AA:BB:CC", mac, 6), "too short");
    CHECK(!observore_mute_parse_mac("ZZ:BB:CC:DD:EE:FF", mac, 6), "non-hex");
    CHECK(!observore_mute_parse_mac("", mac, 6), "empty");

    observore_class_t cls;
    CHECK(observore_mute_parse_class("bodycam", &cls), "class name");
    CHECK(cls == OBSERVORE_CLASS_BODYCAM, "wrong class");
    CHECK(!observore_mute_parse_class("nonsense", &cls), "unknown class");
    CHECK(!observore_mute_parse_class("unknown", &cls),
          "the unknown class must not be addressable by name");
}

/* A crude "is this parseable" check: balanced braces and brackets outside of
 * strings, and no dangling escape.  Enough to catch the failure that actually
 * shipped -- a document that simply stopped mid-object. */
static bool json_balanced(const char *s)
{
    int curly = 0, square = 0;
    bool in_str = false, esc = false;
    for (; *s; s++) {
        if (esc) { esc = false; continue; }
        if (in_str) {
            if (*s == '\\') esc = true;
            else if (*s == '"') in_str = false;
            continue;
        }
        if (*s == '"') in_str = true;
        else if (*s == '{') curly++;
        else if (*s == '}') curly--;
        else if (*s == '[') square++;
        else if (*s == ']') square--;
        if (curly < 0 || square < 0) return false;
    }
    return curly == 0 && square == 0 && !in_str && !esc;
}

static void test_json_builder(void)
{
    banner("json append cursor");

    char buf[128];
    observore_jbuf_t jb;

    observore_jb_init(&jb, buf, sizeof(buf), 2);
    observore_jb_printf(&jb, "{\"a\":%d,\"b\":[", 42);
    observore_jb_printf(&jb, "1,2,3");
    observore_jb_close(&jb, "]}");
    CHECK(strcmp(buf, "{\"a\":42,\"b\":[1,2,3]}") == 0, "got '%s'", buf);
    CHECK(json_balanced(buf), "should be balanced");

    /* Escaping goes in without quotes, so a caller supplies them. */
    observore_jb_init(&jb, buf, sizeof(buf), 2);
    observore_jb_printf(&jb, "{\"n\":\"");
    observore_jb_escape(&jb, "he said \"hi\"\\then\nleft");
    observore_jb_printf(&jb, "\"");
    observore_jb_close(&jb, "}");
    CHECK(json_balanced(buf), "escaped output should stay balanced: %s", buf);
    CHECK(strstr(buf, "\\\"hi\\\"") != NULL, "quotes not escaped: %s", buf);
    CHECK(strstr(buf, "\\n") != NULL, "newline not escaped: %s", buf);

    /* The property the whole cursor exists for: running out of room must yield
     * a SHORT but PARSEABLE document, never a truncated one.  A missing
     * closing bracket is exactly what shipped once and what the browser then
     * refused to parse. */
    char small[48];
    observore_jb_init(&jb, small, sizeof(small), 2);
    observore_jb_printf(&jb, "{\"items\":[");
    int accepted = 0;
    for (int i = 0; i < 100; i++) {
        observore_jb_printf(&jb, "%s{\"index\":%d,\"padding\":\"xxxxxxxx\"}",
                            i ? "," : "", i);
        if (observore_jb_full(&jb)) break;
        accepted++;
    }
    observore_jb_close(&jb, "]}");
    CHECK(observore_jb_full(&jb), "the small buffer should have filled");
    CHECK(accepted < 100, "not everything can have fitted");
    CHECK(json_balanced(small), "a full buffer must still close: '%s'", small);
    CHECK(strlen(small) < sizeof(small), "must not overrun");

    /* A partial write is dropped whole rather than leaving half a token. */
    observore_jb_init(&jb, small, sizeof(small), 2);
    observore_jb_printf(&jb, "{\"x\":\"");
    size_t before = strlen(small);
    observore_jb_printf(&jb, "%s", "an extremely long value that cannot fit at all");
    CHECK(observore_jb_full(&jb), "should be full");
    CHECK(strlen(small) == before, "a rejected write must leave nothing behind");

    /* Escaping must also stop cleanly at the boundary. */
    observore_jb_init(&jb, small, sizeof(small), 2);
    observore_jb_printf(&jb, "{\"x\":\"");
    observore_jb_escape(&jb, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    observore_jb_printf(&jb, "\"");
    observore_jb_close(&jb, "}");
    CHECK(strlen(small) < sizeof(small), "escape must not overrun");

    /* A reserve larger than the buffer must not explode. */
    char tiny[4];
    observore_jb_init(&jb, tiny, sizeof(tiny), 8);
    CHECK(observore_jb_full(&jb), "an impossible reserve should read as full");
    observore_jb_printf(&jb, "anything");
    CHECK(strlen(tiny) == 0, "nothing should have been written");
}

static const observore_notify_header_t *hdr(const observore_notify_request_t *r,
                                            const char *name)
{
    for (size_t i = 0; i < r->header_count; i++) {
        if (strcmp(r->headers[i].name, name) == 0) {
            return &r->headers[i];
        }
    }
    return NULL;
}

static void test_notify_providers(void)
{
    banner("notification providers");

    observore_notify_request_t r;

    /* --- Gotify: JSON body, token in a header, numeric priority --- */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_GOTIFY,
                                 "https://gotify.example.com", "TOK", NULL,
                                 "bodycam detected", "Axon", 
                                 OBSERVORE_URGENCY_URGENT, &r), "gotify build");
    CHECK(strcmp(r.url, "https://gotify.example.com/message") == 0,
          "url '%s'", r.url);
    CHECK(hdr(&r, "X-Gotify-Key") &&
          strcmp(hdr(&r, "X-Gotify-Key")->value, "TOK") == 0, "token header");
    CHECK(json_balanced(r.body), "body should be valid JSON: %s", r.body);
    CHECK(strstr(r.body, "\"priority\":8") != NULL, "urgent -> 8: %s", r.body);

    /* A trailing slash must not produce a doubled path. */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_GOTIFY,
                                 "https://gotify.example.com/", "T", NULL,
                                 "t", "m", OBSERVORE_URGENCY_LOW, &r), "build");
    CHECK(strcmp(r.url, "https://gotify.example.com/message") == 0,
          "trailing slash: '%s'", r.url);
    CHECK(strstr(r.body, "\"priority\":2") != NULL, "low -> 2");

    /* --- ntfy: plain body, title and priority as headers --- */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_NTFY,
                                 "https://ntfy.sh/my-topic", "", NULL,
                                 "tracker detected", "Find My tracker\n-50 dBm",
                                 OBSERVORE_URGENCY_HIGH, &r), "ntfy build");
    CHECK(strcmp(r.url, "https://ntfy.sh/my-topic") == 0, "url '%s'", r.url);
    CHECK(strcmp(r.content_type, "text/plain") == 0, "content type");
    CHECK(hdr(&r, "Title") &&
          strcmp(hdr(&r, "Title")->value, "tracker detected") == 0, "title");
    CHECK(hdr(&r, "Priority") &&
          strcmp(hdr(&r, "Priority")->value, "high") == 0, "priority name");
    CHECK(strcmp(r.body, "Find My tracker\n-50 dBm") == 0, "body is the message");
    /* No token means no Authorization header at all, not an empty one. */
    CHECK(hdr(&r, "Authorization") == NULL, "no empty auth header");

    CHECK(observore_notify_build(OBSERVORE_PROVIDER_NTFY, "https://ntfy.sh/t",
                                 "tk_secret", NULL, "t", "m",
                                 OBSERVORE_URGENCY_URGENT, &r), "build");
    CHECK(hdr(&r, "Authorization") &&
          strcmp(hdr(&r, "Authorization")->value, "Bearer tk_secret") == 0,
          "bearer token");
    CHECK(strcmp(hdr(&r, "Priority")->value, "urgent") == 0, "urgent");

    /* --- Pushover: form body, needs a user key, default URL --- */
    CHECK(!observore_notify_build(OBSERVORE_PROVIDER_PUSHOVER, "", "APP", NULL,
                                  "t", "m", OBSERVORE_URGENCY_NORMAL, &r),
          "pushover without a user key must be refused");
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_PUSHOVER, "", "APP", "USER",
                                 "t", "m", OBSERVORE_URGENCY_NORMAL, &r),
          "pushover build");
    CHECK(strcmp(r.url, "https://api.pushover.net/1/messages.json") == 0,
          "default url used when none configured: '%s'", r.url);
    CHECK(strstr(r.body, "token=APP") && strstr(r.body, "user=USER"),
          "credentials in the form body: %s", r.body);
    CHECK(strstr(r.body, "priority=0") != NULL, "normal -> 0");

    /* An advertised name is attacker-chosen text going into a form body, so
     * an unescaped '&' would inject a field. */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_PUSHOVER, "", "APP", "USER",
                                 "x", "evil&priority=2&x= y", 
                                 OBSERVORE_URGENCY_LOW, &r), "build");
    CHECK(strstr(r.body, "evil%26priority%3D2") != NULL,
          "form injection not escaped: %s", r.body);
    CHECK(strstr(r.body, "%20y") != NULL, "space not escaped: %s", r.body);
    /* Exactly one priority field, and it is ours. */
    const char *p1 = strstr(r.body, "&priority=");
    CHECK(p1 && strstr(p1 + 1, "&priority=") == NULL,
          "more than one priority field: %s", r.body);

    /* Names round-trip, and an unknown one is refused. */
    observore_provider_t pv;
    CHECK(observore_provider_from_name("ntfy", &pv) &&
          pv == OBSERVORE_PROVIDER_NTFY, "name lookup");
    CHECK(!observore_provider_from_name("carrier-pigeon", &pv), "unknown provider");
    for (int i = 0; i < OBSERVORE_PROVIDER_MAX; i++) {
        CHECK(observore_provider_from_name(observore_provider_name(i), &pv) &&
              (int)pv == i, "round trip for %s", observore_provider_name(i));
    }
    CHECK(observore_provider_needs_user(OBSERVORE_PROVIDER_PUSHOVER),
          "pushover needs a user key");
    CHECK(!observore_provider_needs_user(OBSERVORE_PROVIDER_GOTIFY),
          "gotify does not");
}

/* ------------------------------------------------------------------ */
/* WPS information elements                                           */
/* ------------------------------------------------------------------ */

/* Build a vendor-specific IE carrying WPS attributes. */
static size_t wps_ie(uint8_t *buf, const uint16_t *types, const char **vals, size_t n)
{
    size_t body = 4;
    buf[0] = 0xDD;
    buf[2] = 0x00; buf[3] = 0x50; buf[4] = 0xF2; buf[5] = 0x04;
    for (size_t i = 0; i < n; i++) {
        size_t vl = strlen(vals[i]);
        buf[2 + body + 0] = (uint8_t)(types[i] >> 8);
        buf[2 + body + 1] = (uint8_t)(types[i] & 0xFF);
        buf[2 + body + 2] = (uint8_t)(vl >> 8);
        buf[2 + body + 3] = (uint8_t)(vl & 0xFF);
        memcpy(&buf[2 + body + 4], vals[i], vl);
        body += 4 + vl;
    }
    buf[1] = (uint8_t)body;
    return 2 + body;
}

static void test_wps(void)
{
    uint8_t buf[512];
    observore_wps_t w;

    banner("WPS: an access point naming itself");
    const uint16_t t3[] = {0x1021, 0x1023, 0x1011};
    const char *v3[] = {"Hikvision", "DS-2CD2042WD", "Front Door"};
    size_t n = wps_ie(buf, t3, v3, 3);
    CHECK(observore_wps_from_ies(buf, n, &w), "a WPS element is found");
    CHECK(strcmp(w.manufacturer, "Hikvision") == 0, "manufacturer, got \"%s\"", w.manufacturer);
    CHECK(strcmp(w.model, "DS-2CD2042WD") == 0, "model, got \"%s\"", w.model);
    CHECK(strcmp(w.device_name, "Front Door") == 0, "device name, got \"%s\"", w.device_name);

    banner("WPS: frames that are not well behaved");
    CHECK(!observore_wps_from_ies(NULL, 10, &w), "a null frame is not a crash");
    CHECK(observore_wps_empty(&w), "and leaves the result empty");

    /* An attribute claiming to be longer than the element containing it. */
    n = wps_ie(buf, t3, v3, 1);
    buf[2 + 4 + 2] = 0xFF; buf[2 + 4 + 3] = 0xFF;
    observore_wps_from_ies(buf, n, &w);
    CHECK(observore_wps_empty(&w), "an over-long attribute is refused, not read");

    /* An element claiming to be longer than the frame containing it. */
    n = wps_ie(buf, t3, v3, 1);
    buf[1] = 0xFF;
    observore_wps_from_ies(buf, n, &w);
    CHECK(observore_wps_empty(&w), "an over-long element is refused");

    /* Control characters and a quote heading for a JSON document. */
    const uint16_t t1[] = {0x1021};
    const char *nasty[] = {"Ac\x01me\x1b[31m\"x"};
    n = wps_ie(buf, t1, nasty, 1);
    observore_wps_from_ies(buf, n, &w);
    CHECK(strcmp(w.manufacturer, "Acme[31m\"x") == 0,
          "control bytes are dropped, got \"%s\"", w.manufacturer);

    /* Longer than the field, and padded, as real hardware often is. */
    const char *longv[] = {"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789   "};
    n = wps_ie(buf, t1, longv, 1);
    observore_wps_from_ies(buf, n, &w);
    CHECK(strlen(w.manufacturer) == 32, "truncated to the field, got %zu", strlen(w.manufacturer));

    const char *padded[] = {"Ubiquiti   "};
    n = wps_ie(buf, t1, padded, 1);
    observore_wps_from_ies(buf, n, &w);
    CHECK(strcmp(w.manufacturer, "Ubiquiti") == 0, "trailing blanks trimmed, got \"%s\"", w.manufacturer);

    banner("WPS: a device that names itself is classified");
    {
        /* The case the OUI cannot reach: an address block we do not know,
         * and a beacon that says what the device is anyway. */
        const uint16_t tk[] = {0x1021, 0x1023};
        const char *cam[] = {"Axon Enterprise", "Fleet 3 ALPR"};
        size_t cn = wps_ie(buf, tk, cam, 2);
        observore_wps_t cw;
        CHECK(observore_wps_from_ies(buf, cn, &cw), "the element parses");

        uint8_t unknown_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        observore_observation_t obs = {
            .mac = unknown_mac, .src = OBSERVORE_SRC_WIFI_SNIFF,
            .rssi = -50, .channel = 6, .ssid = NULL, .wps = &cw,
        };
        observore_event_t ev;
        CHECK(observore_classify(&obs, &ev), "an unknown MAC is still classified");
        CHECK(ev.cls == OBSERVORE_CLASS_BODYCAM, "by what it called itself, got %s",
              observore_class_name(ev.cls));
        CHECK(strstr(ev.detail, "Axon") != NULL,
              "and the detail names it, got \"%s\"", ev.detail);
    }

    banner("WPS: elements that are not WPS");
    uint8_t other[] = {0x00, 0x04, 'h','o','m','e',      /* SSID */
                       0xDD, 0x04, 0x00, 0x50, 0xF2, 0x01};  /* WPA, not WPS */
    CHECK(!observore_wps_from_ies(other, sizeof(other), &w), "a WPA element is not mistaken for WPS");
}

/* The two names that were actually in the air when this device failed to
 * classify them, plus the substring hazard that kept a third keyword out. */
/* A phone's address rotates every fifteen minutes. Keyed by address alone the
 * tracker saw each rotation as a new device -- a neighbour's handset produced a
 * fresh follower alert every quarter hour, and a device that genuinely stayed
 * two hours was invisible because none of its addresses lasted long enough. */
/* The device that set these rules: a static random address at -82 to -90 dBm,
 * fading below the floor and returning, announced afresh every time. */
static void test_edge_of_range_noise(void)
{
    banner("edge-of-range noise");

    uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    const uint8_t far_mac[6]  = {0xFE, 0xF2, 0xB3, 0x9D, 0x42, 0x46};
    const uint8_t near_mac[6] = {0xFE, 0xF2, 0xB3, 0x00, 0x00, 0x01};
    observore_observation_t far = {.mac = far_mac, .src = OBSERVORE_SRC_BLE, .rssi = -83,
                                   .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
    observore_observation_t near = far; near.mac = near_mac; near.rssi = -60;
    observore_status_t st;

    /* A random address that has never come closer than -83 is never a
     * follower, however long it persists. */
    observore_track_init();
    for (int i = 0; i < 12; i++) observore_track_observe(&far, SECS(i * 60));
    observore_track_status(&st, SECS(720));
    CHECK(st.device_count == 0, "a -83 dBm random address is not a follower, got %u",
          st.device_count);

    /* The same address, once seen at -60, is -- persistence then counts. */
    far.rssi = -60; observore_track_observe(&far, SECS(780)); far.rssi = -83;
    observore_track_observe(&far, SECS(840));
    observore_track_status(&st, SECS(840));
    CHECK(st.device_count == 1, "having once been close, it counts, got %u", st.device_count);

    /* A public address at -83 keeps the old rule: a vendor can be reasoned with. */
    observore_track_init();
    const uint8_t pub[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    observore_observation_t p = far; p.mac = pub; p.addr_random = false; p.rssi = -83;
    for (int i = 0; i < 4; i++) observore_track_observe(&p, SECS(i * 120));
    observore_track_status(&st, SECS(360));
    CHECK(st.device_count == 1, "a public address at -83 is still a follower, got %u",
          st.device_count);

    /* Announced once, faded out, came back: tracked and scored again, but not
     * handed to the digest a second time. */
    observore_track_init();
    observore_event_t out[4];
    for (int i = 0; i < 4; i++) observore_track_observe(&near, SECS(i * 120));
    CHECK(observore_track_drain_new(out, 4) == 1, "first appearance is announced");
    /* Gone for 40 minutes: past the 30-minute TTL, the slot is evicted. */
    observore_track_tick(SECS(360 + 40 * 60));
    observore_track_status(&st, SECS(360 + 40 * 60));
    CHECK(st.device_count == 0, "evicted while away, got %u", st.device_count);
    /* Back, and persistent again. */
    int64_t t = SECS(360 + 40 * 60);
    for (int i = 0; i < 4; i++) observore_track_observe(&near, t + SECS(i * 120));
    observore_track_status(&st, t + SECS(360));
    CHECK(st.device_count == 1, "tracked again on return, got %u", st.device_count);
    CHECK(st.score > 0, "and scored again: the level stays honest");
    CHECK(observore_track_drain_new(out, 4) == 0, "but not announced again within six hours");

    /* Seven hours later it is news again. */
    observore_track_tick(t + SECS(7 * 3600));
    t += SECS(7 * 3600);
    for (int i = 0; i < 4; i++) observore_track_observe(&near, t + SECS(i * 120));
    CHECK(observore_track_drain_new(out, 4) == 1, "after the memory expires it is announced");

    /* A signature class is announced every time it comes back. A body camera
     * at 20:00 and again at 22:00 is two pieces of news, not one; only the
     * persistence inference is the same inference twice. */
    observore_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x00, 0x00, 0x01};   /* Axon OUI */
    observore_observation_t bwc = {.mac = axon, .src = OBSERVORE_SRC_BLE, .rssi = -70,
                                   .adv = adv, .adv_len = sizeof(adv)};
    observore_track_observe(&bwc, SECS(0));
    CHECK(observore_track_drain_new(out, 4) == 1 && out[0].cls == OBSERVORE_CLASS_BODYCAM,
          "a body camera is announced on sight");
    observore_track_tick(SECS(40 * 60));
    observore_track_observe(&bwc, SECS(40 * 60));
    CHECK(observore_track_drain_new(out, 4) == 1,
          "and announced again when it comes back forty minutes later");

    /* A rotating address is remembered by its advert, not its address. */
    observore_track_init();
    const uint8_t r1[6] = {0x4A, 1, 1, 1, 1, 1}, r2[6] = {0x5E, 2, 2, 2, 2, 2};
    observore_observation_t a = near; a.mac = r1;
    for (int i = 0; i < 4; i++) observore_track_observe(&a, SECS(i * 120));
    CHECK(observore_track_drain_new(out, 4) == 1, "rotating device announced once");
    observore_track_tick(SECS(360 + 40 * 60)); t = SECS(360 + 40 * 60);
    a.mac = r2;                                   /* new address, same advert */
    for (int i = 0; i < 4; i++) observore_track_observe(&a, t + SECS(i * 120));
    CHECK(observore_track_drain_new(out, 4) == 0,
          "the same advert under a new address is not announced again");
}

static void test_rotation_continuity(void)
{
    banner("following a device across an address rotation");

    /* A stable advert: flags plus a 16-bit service UUID. The fingerprint hashes
     * the structure and the UUID, neither of which changes when the address
     * does. */
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE};
    const uint8_t a1[6] = {0x4A, 0x11, 0x11, 0x11, 0x11, 0x11};  /* random (0x4A) */
    const uint8_t a2[6] = {0x5E, 0x22, 0x22, 0x22, 0x22, 0x22};  /* random (0x5E) */
    const uint8_t a3[6] = {0x6A, 0x33, 0x33, 0x33, 0x33, 0x33};

    observore_observation_t o1 = {.mac = a1, .src = OBSERVORE_SRC_BLE, .rssi = -60,
                                  .addr_random = true, .adv = adv, .adv_len = sizeof(adv)};
    observore_observation_t o2 = o1; o2.mac = a2;
    observore_observation_t o3 = o1; o3.mac = a3;
    observore_status_t st;

    /* One device, seen under its first address, becomes a follower. */
    observore_track_init();
    observore_track_observe(&o1, SECS(0));
    observore_track_observe(&o1, SECS(100));
    observore_track_observe(&o1, SECS(200));
    CHECK(observore_track_observe(&o1, SECS(310)), "follower under the first address");
    observore_track_status(&st, SECS(310));
    CHECK(st.device_count == 1, "one device, got %u", st.device_count);

    /* It rotates. Thirty seconds of silence, then the same advert from a new
     * address at the same strength. That must be the same device: still one
     * device, hits carried over, and it is not a new detection. */
    observore_track_observe(&o2, SECS(340));
    observore_track_status(&st, SECS(340));
    CHECK(st.device_count == 1, "a rotation must not create a device, got %u",
          st.device_count);
    observore_event_t snap[8];
    size_t n = observore_track_snapshot(snap, 8);
    CHECK(n == 1, "one classified device after rotation, got %zu", n);
    CHECK(n == 1 && memcmp(snap[0].mac, a2, 6) == 0, "the slot now carries the new address");
    CHECK(n == 1 && snap[0].hits == 5, "hits carry over, got %u", n ? snap[0].hits : 0);
    CHECK(n == 1 && snap[0].rotations == 1, "one rotation counted, got %u",
          n ? snap[0].rotations : 0);
    CHECK(n == 1 && snap[0].first_seen_us == SECS(0), "first sighting is preserved");
    CHECK(n == 1 && strstr(snap[0].label, "rotated 1x") != NULL,
          "the label says it rotated: %s", n ? snap[0].label : "");

    /* The old address is gone: a sighting of it now would be a new device. */
    CHECK(observore_track_snapshot(snap, 8) == 1, "still one");

    /* A second rotation, twenty minutes on. */
    observore_track_observe(&o3, SECS(340 + 15 * 60 + 30));
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].rotations == 2, "two rotations, got %u", n ? snap[0].rotations : 0);
    CHECK(n == 1 && memcmp(snap[0].mac, a3, 6) == 0, "carries the third address");

    /* Not the same device: different fingerprint. */
    observore_track_init();
    observore_track_observe(&o1, SECS(0));
    uint8_t other_adv[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xFA, 0xFF};
    observore_observation_t different = o2;
    different.adv = other_adv; different.adv_len = sizeof(other_adv);
    observore_track_observe(&different, SECS(30));
    observore_track_status(&st, SECS(30));
    CHECK(st.device_count == 0, "unclassified pair should not count yet");
    /* Count slots by giving both enough to classify. */
    observore_track_observe(&o1, SECS(100)); observore_track_observe(&o1, SECS(200));
    observore_track_observe(&o1, SECS(310));
    observore_track_observe(&different, SECS(130)); observore_track_observe(&different, SECS(230));
    observore_track_observe(&different, SECS(340));
    observore_track_status(&st, SECS(340));
    CHECK(st.device_count == 2, "different adverts are different devices, got %u",
          st.device_count);

    /* Not the same device: the old address is still transmitting. Two identical
     * handsets in one room must stay two devices. */
    observore_track_init();
    observore_track_observe(&o1, SECS(0));
    observore_track_observe(&o1, SECS(5));
    observore_track_observe(&o2, SECS(7));      /* a1 heard 2 s ago: not quiet */
    observore_track_observe(&o1, SECS(100)); observore_track_observe(&o1, SECS(200));
    observore_track_observe(&o1, SECS(310));
    observore_track_observe(&o2, SECS(107)); observore_track_observe(&o2, SECS(207));
    observore_track_observe(&o2, SECS(317));
    observore_track_status(&st, SECS(317));
    CHECK(st.device_count == 2, "two concurrent identical devices stay two, got %u",
          st.device_count);

    /* Not the same device: too far away in signal. */
    observore_track_init();
    observore_track_observe(&o1, SECS(0));
    observore_observation_t far = o2; far.rssi = -85;
    observore_track_observe(&far, SECS(30));
    n = observore_track_snapshot(snap, 8);
    observore_track_observe(&o1, SECS(100)); observore_track_observe(&o1, SECS(200));
    observore_track_observe(&o1, SECS(310));
    observore_track_status(&st, SECS(310));
    CHECK(st.device_count == 1, "a 25 dB weaker sighting is a different device");
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && memcmp(snap[0].mac, a1, 6) == 0 && snap[0].rotations == 0,
          "and the original was not rewritten");

    /* Not the same device: silent for longer than the window. */
    observore_track_init();
    observore_track_observe(&o1, SECS(0));
    observore_track_observe(&o2, SECS(25 * 60));
    observore_track_observe(&o2, SECS(25 * 60 + 100));
    observore_track_observe(&o2, SECS(25 * 60 + 200));
    observore_track_observe(&o2, SECS(25 * 60 + 310));
    n = observore_track_snapshot(snap, 8);
    CHECK(n == 1 && snap[0].rotations == 0 && snap[0].hits == 4,
          "beyond the window it starts fresh: rotations %u hits %u",
          n ? snap[0].rotations : 0, n ? snap[0].hits : 0);

    /* Not the same device: a public address never rotates, so never inherits. */
    observore_track_init();
    const uint8_t pub1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t pub2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};
    observore_observation_t p1 = o1; p1.mac = pub1; p1.addr_random = false;
    observore_observation_t p2 = o1; p2.mac = pub2; p2.addr_random = false;
    observore_track_observe(&p1, SECS(0));
    observore_track_observe(&p2, SECS(30));
    observore_track_observe(&p1, SECS(100)); observore_track_observe(&p1, SECS(200));
    observore_track_observe(&p1, SECS(310));
    observore_track_observe(&p2, SECS(130)); observore_track_observe(&p2, SECS(230));
    observore_track_observe(&p2, SECS(340));
    observore_track_status(&st, SECS(340));
    CHECK(st.device_count == 2, "public addresses are never merged, got %u",
          st.device_count);
}

static void test_webhook_and_telegram(void)
{
    banner("webhook and telegram");
    observore_notify_request_t r;

    /* Webhook: posted to the URL exactly as given, JSON body, bearer token
     * only when there is one. The URL is the identity for most webhooks, so
     * even a query string has to survive untouched. */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_WEBHOOK,
                                 "https://ha.local/api/webhook/abc123?x=1", "", "",
                                 "alert: 2 findings", "line one\nline two",
                                 OBSERVORE_URGENCY_HIGH, &r),
          "webhook builds without a token");
    CHECK(strcmp(r.url, "https://ha.local/api/webhook/abc123?x=1") == 0,
          "URL untouched: %s", r.url);
    CHECK(strcmp(r.content_type, "application/json") == 0, "json content type");
    CHECK(r.header_count == 0, "no auth header without a token, got %zu", r.header_count);
    CHECK(strstr(r.body, "\"source\":\"observore\"") != NULL, "names its source");
    CHECK(strstr(r.body, "\"urgency\":\"high\"") != NULL, "urgency as a word: %s", r.body);
    CHECK(strstr(r.body, "\"title\":\"alert: 2 findings\"") != NULL, "title field");
    CHECK(strstr(r.body, "\"message\":\"line one\\nline two\"") != NULL,
          "newline escaped, not raw: %s", r.body);

    CHECK(observore_notify_build(OBSERVORE_PROVIDER_WEBHOOK,
                                 "http://192.168.1.5:8080/hook", "s3cret", "",
                                 "t", "m", OBSERVORE_URGENCY_LOW, &r),
          "webhook builds over plain http on the LAN");
    CHECK(r.header_count == 1 && strcmp(r.headers[0].name, "Authorization") == 0 &&
          strcmp(r.headers[0].value, "Bearer s3cret") == 0,
          "bearer token when given");
    CHECK(strstr(r.body, "\"urgency\":\"low\"") != NULL, "low urgency");

    /* A message with a quote in it must not break the JSON. */
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_WEBHOOK, "https://x/y", "", "",
                                 "t", "say \"hi\"", OBSERVORE_URGENCY_NORMAL, &r),
          "builds with a quote");
    CHECK(strstr(r.body, "say \\\"hi\\\"") != NULL, "quote escaped: %s", r.body);

    /* Telegram: token in the path, chat id in the body, title and message
     * joined by a newline, and a low digest arrives silently. */
    CHECK(!observore_notify_build(OBSERVORE_PROVIDER_TELEGRAM, "", "", "12345",
                                  "t", "m", OBSERVORE_URGENCY_NORMAL, &r),
          "telegram needs a bot token");
    CHECK(!observore_notify_build(OBSERVORE_PROVIDER_TELEGRAM, "", "123:ABC", "",
                                  "t", "m", OBSERVORE_URGENCY_NORMAL, &r),
          "telegram needs a chat id");
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_TELEGRAM, "", "123:ABC-def", "98765",
                                 "alert: 1 finding", "drone 18:65:CF:00:23:EF -54 dBm",
                                 OBSERVORE_URGENCY_NORMAL, &r),
          "telegram builds with the default host");
    CHECK(strcmp(r.url, "https://api.telegram.org/bot123:ABC-def/sendMessage") == 0,
          "bot api path: %s", r.url);
    CHECK(strstr(r.body, "\"chat_id\":\"98765\"") != NULL, "chat id: %s", r.body);
    CHECK(strstr(r.body, "\"text\":\"alert: 1 finding\\ndrone 18:65:CF:00:23:EF -54 dBm\"") != NULL,
          "title, newline, message: %s", r.body);
    CHECK(strstr(r.body, "\"disable_notification\":false") != NULL, "normal makes a sound");

    CHECK(observore_notify_build(OBSERVORE_PROVIDER_TELEGRAM, "", "123:ABC", "9",
                                 "t", "m", OBSERVORE_URGENCY_LOW, &r),
          "low builds");
    CHECK(strstr(r.body, "\"disable_notification\":true") != NULL,
          "a low digest arrives silently");

    /* The full-size digest still fits both. */
    char title[OBSERVORE_DIGEST_TITLE_LEN];
    char body[OBSERVORE_DIGEST_BODY_LEN];
    observore_digest_entry_t many[10];
    static char lines[10][OBSERVORE_DIGEST_LINE_LEN];
    for (size_t i = 0; i < 10; i++) {
        snprintf(lines[i], sizeof(lines[i]), "follower EE:00:00:00:00:%02zu  -60 dBm", i);
        many[i].rank = 4; many[i].rssi = -60; many[i].cls = "follower"; many[i].line = lines[i];
    }
    observore_digest_build(many, 10, "alert", title, sizeof(title), body, sizeof(body));
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_WEBHOOK, "https://x/y", "tok", "",
                                 title, body, OBSERVORE_URGENCY_URGENT, &r),
          "full digest fits a webhook");
    CHECK(r.body[strlen(r.body) - 1] == '}', "webhook JSON is closed, not truncated");
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_TELEGRAM, "", "123:ABC", "9",
                                 title, body, OBSERVORE_URGENCY_URGENT, &r),
          "full digest fits telegram");
    CHECK(r.body[strlen(r.body) - 1] == '}', "telegram JSON is closed, not truncated");
}

static void test_heapwatch(void)
{
    banner("heap watch");

    observore_heapwatch_init();
    CHECK(observore_heapwatch_latest() == NULL, "nothing recorded at start");

    /* The first reading is always a new low. */
    CHECK(observore_heapwatch_note(40000, 30000, 0, "patrol", 1000000),
          "first reading is recorded");
    /* A drop smaller than the step is not an event: a slow slide should
     * record milestones, not every byte. */
    CHECK(!observore_heapwatch_note(39000, 30000, 0, "patrol", 2000000),
          "a 1 KB drop is below the step");
    CHECK(observore_heapwatch_note(30000, 20000, 3, "uplink", 3000000),
          "a 10 KB drop is an event");
    /* Going back up is never an event; the mark only moves down. */
    CHECK(!observore_heapwatch_note(45000, 30000, 0, "patrol", 4000000),
          "recovery is not recorded");

    const observore_heap_event_t *l = observore_heapwatch_latest();
    CHECK(l && l->free_min == 30000, "latest is the deepest, got %u", l ? l->free_min : 0);
    CHECK(l && strcmp(l->mode, "uplink") == 0, "mode travels with it: %s", l ? l->mode : "?");
    CHECK(l && l->queued == 3, "queue depth travels with it");
    CHECK(l && l->at_us == 3000000, "and so does the moment");

    observore_heap_event_t ev[OBSERVORE_HEAPWATCH_EVENTS];
    size_t n = observore_heapwatch_events(ev, OBSERVORE_HEAPWATCH_EVENTS);
    CHECK(n == 2, "two events, got %zu", n);
    CHECK(ev[0].free_min == 40000 && ev[1].free_min == 30000, "oldest first");

    /* Fill past capacity: the oldest fall off, order is preserved. */
    observore_heapwatch_init();
    for (uint32_t i = 0; i < OBSERVORE_HEAPWATCH_EVENTS + 3; i++) {
        observore_heapwatch_note(100000 - i * 5000, 1000, i, "patrol", (int64_t)i);
    }
    n = observore_heapwatch_events(ev, OBSERVORE_HEAPWATCH_EVENTS);
    CHECK(n == OBSERVORE_HEAPWATCH_EVENTS, "ring holds %d, got %zu",
          OBSERVORE_HEAPWATCH_EVENTS, n);
    CHECK(ev[0].at_us == 3, "the three oldest fell off, got %lld", (long long)ev[0].at_us);
    CHECK(ev[n-1].at_us == OBSERVORE_HEAPWATCH_EVENTS + 2, "the newest is last");
    for (size_t i = 1; i < n; i++) {
        CHECK(ev[i].free_min < ev[i-1].free_min, "each event is lower than the last");
    }

    /* Asking for fewer than held returns the most recent, still in order. */
    n = observore_heapwatch_events(ev, 3);
    CHECK(n == 3 && ev[2].at_us == OBSERVORE_HEAPWATCH_EVENTS + 2,
          "a short read ends at the newest");
}

static void test_version(void)
{
    banner("firmware versions");

    observore_version_t v;
    CHECK(observore_version_parse("v0.5.0", &v), "a plain tag should parse");
    CHECK(v.major == 0 && v.minor == 5 && v.patch == 0, "0.5.0 read wrong");
    CHECK(!v.dev, "a tag is not a dev build");

    /* What ESP-IDF actually stamps into a build made after the tag. */
    CHECK(observore_version_parse("v0.5.0-3-gce8e56e", &v), "git describe form");
    CHECK(v.major == 0 && v.minor == 5 && v.patch == 0, "numbers survive the suffix");
    CHECK(v.dev, "a suffix marks a build after the tag");

    CHECK(observore_version_parse("0.5.0", &v), "the v is optional");
    CHECK(!observore_version_parse("", &v), "empty is not a version");
    CHECK(!observore_version_parse("garbage", &v), "nor is a word");
    CHECK(!observore_version_parse("v1.2", &v), "nor is a two-part number");
    CHECK(!observore_version_parse(NULL, &v), "nor is nothing at all");

    /* The direction that matters: an update is only offered when it goes up. */
    CHECK(observore_version_is_newer("v0.6.0", "v0.5.0"), "minor bump is newer");
    CHECK(observore_version_is_newer("v0.5.1", "v0.5.0"), "patch bump is newer");
    CHECK(observore_version_is_newer("v1.0.0", "v0.9.9"), "major bump is newer");
    CHECK(!observore_version_is_newer("v0.5.0", "v0.5.0"), "same is not newer");
    CHECK(!observore_version_is_newer("v0.4.0", "v0.5.0"),
          "an older release must never be offered as an update");
    CHECK(!observore_version_is_newer("v0.9.9", "v1.0.0"),
          "9 does not outrank 10 in a component-wise compare");

    /* A device running a build cut after the tag is ahead of that tag, so the
     * published release must not be offered back to it as an upgrade. */
    CHECK(!observore_version_is_newer("v0.5.0", "v0.5.0-3-gce8e56e"),
          "a dev build is ahead of the tag it came from");
    CHECK(observore_version_is_newer("v0.6.0", "v0.5.0-3-gce8e56e"),
          "but a real new release still is newer");

    /* Anything unreadable means no update, never a guess. */
    CHECK(!observore_version_is_newer("not-a-version", "v0.5.0"), "unreadable remote");
    CHECK(!observore_version_is_newer("v9.9.9", "not-a-version"), "unreadable local");

    /* Reading the version out of the boards.json this project publishes. */
    const char *doc =
        "{\"version\":\"v0.5.0\",\"boards\":["
        "{\"id\":\"xiao-esp32s3\",\"manifest\":\"manifest-xiao-esp32s3.json\"}]}";
    char buf[32];
    CHECK(observore_json_string_field(doc, "version", buf, sizeof(buf)),
          "version field should be found");
    CHECK(strcmp(buf, "v0.5.0") == 0, "got '%s'", buf);
    CHECK(observore_json_string_field(doc, "manifest", buf, sizeof(buf)),
          "a nested key is still findable by name");
    CHECK(!observore_json_string_field(doc, "absent", buf, sizeof(buf)),
          "a missing key is a miss, not a crash");
    CHECK(!observore_json_string_field("{\"version\":123}", "version",
                                       buf, sizeof(buf)),
          "a number is not a version string");
    CHECK(!observore_json_string_field("{\"version\":\"unterminated",
                                       "version", buf, sizeof(buf)),
          "an unterminated string must not run off the document");
    char tiny[4];
    CHECK(!observore_json_string_field(doc, "version", tiny, sizeof(tiny)),
          "a value that does not fit is a miss, not a truncation");
}

static void test_name_keywords(void)
{
    banner("ssid keywords: names seen on hardware");

    char label[24];

    CHECK(observore_ssid_is_suspicious("Setup UVC G3 Micro (6AEB)",
                                       label, sizeof(label)),
          "a UniFi camera in setup mode should match");
    CHECK(strcmp(label, "UniFi camera") == 0,
          "labelled by what it is, got '%s'", label);

    CHECK(observore_ssid_is_suspicious("HolyStoneGIM-b79437D",
                                       label, sizeof(label)),
          "a HolyStone control link should match");
    CHECK(strcmp(label, "HolyStone") == 0, "got '%s'", label);

    /* The class matters as much as the match: a camera reported as a drone
     * would rank in the wrong place in a digest. */
    const uint8_t bssid[6] = {0xB6, 0xFB, 0xE4, 0x7E, 0x6A, 0xEB};
    observore_observation_t obs = {
        .mac = bssid, .src = OBSERVORE_SRC_WIFI_SNIFF, .rssi = -37,
        .ssid = "Setup UVC G3 Micro (6AEB)",
    };
    observore_event_t ev;
    CHECK(observore_classify(&obs, &ev), "the camera should classify");
    CHECK(ev.cls == OBSERVORE_CLASS_CAMERA,
          "UVC is a camera, got class %d", (int)ev.cls);

    obs.ssid = "HolyStoneGIM-b79437D";
    CHECK(observore_classify(&obs, &ev), "the drone should classify");
    CHECK(ev.cls == OBSERVORE_CLASS_DRONE,
          "HolyStone is a drone, got class %d", (int)ev.cls);

    /* Matching is case insensitive, which is why the table is lower case. */
    CHECK(observore_ssid_is_suspicious("setup uvc g3 micro", label, sizeof(label)),
          "lower case matches too");
    CHECK(observore_ssid_is_suspicious("DJI-Mini3Pro-1a2b", label, sizeof(label)),
          "an anchored DJI prefix matches");

    /* "arlo" was left out on purpose: it is a substring of ordinary words, and
     * a keyword that fires on somebody's name is worse than a missing brand. */
    CHECK(!observore_ssid_is_suspicious("Carlos iPhone", label, sizeof(label)),
          "a personal SSID must not be read as a camera");
    CHECK(!observore_ssid_is_suspicious("Camden House", label, sizeof(label)),
          "nor must a place name");
    CHECK(!observore_ssid_is_suspicious("BT-HomeHub-8823", label, sizeof(label)),
          "nor an ordinary router");
}

static void test_digest(void)
{
    char title[OBSERVORE_DIGEST_TITLE_LEN];
    char body[OBSERVORE_DIGEST_BODY_LEN];

    /* Ranked by class points, and within a class by signal, closest first.
     * Deliberately handed to the builder in the wrong order. */
    observore_digest_entry_t e[] = {
        {1, -40, "camera",  "camera  AA:00:00:00:00:01  -40 dBm"},
        {4, -80, "follower","follower BB:00:00:00:00:02  -80 dBm"},
        {5, -70, "bodycam", "bodycam CC:00:00:00:00:03  -70 dBm"},
        {4, -50, "follower","follower DD:00:00:00:00:04  -50 dBm"},
    };
    size_t n = observore_digest_build(e, 4, "alert",
                                      title, sizeof(title), body, sizeof(body));
    CHECK(n == 4, "all four findings should fit, got %zu", n);
    CHECK(e[0].rank == 5, "bodycam outranks everything else");
    CHECK(e[1].rank == 4 && e[1].rssi == -50,
          "the closer follower leads the more distant one, got %d", e[1].rssi);
    CHECK(e[2].rank == 4 && e[2].rssi == -80, "the distant follower follows");
    CHECK(e[3].rank == 1, "camera is last, as its points say");

    CHECK(strstr(title, "alert: ") == title, "headline leads the title: %s", title);
    CHECK(strstr(title, "4 findings") != NULL, "title counts findings: %s", title);
    CHECK(strstr(title, "1 bodycam") != NULL, "census names singular: %s", title);
    CHECK(strstr(title, "2 followers") != NULL, "census pluralises: %s", title);
    CHECK(strstr(body, "bodycam CC") < strstr(body, "camera  AA"),
          "body follows the ranking");

    /* More findings than a digest can carry. The tail must be accounted for,
     * not dropped quietly. */
    observore_digest_entry_t many[10];
    static char lines[10][OBSERVORE_DIGEST_LINE_LEN];
    for (size_t i = 0; i < 10; i++) {
        snprintf(lines[i], sizeof(lines[i]),
                 "follower EE:00:00:00:00:%02zu  -60 dBm", i);
        many[i].rank = 4;
        many[i].rssi = -60;
        many[i].cls  = "follower";
        many[i].line = lines[i];
    }
    n = observore_digest_build(many, 10, NULL,
                               title, sizeof(title), body, sizeof(body));
    CHECK(n == OBSERVORE_DIGEST_MAX_LINES,
          "body caps at %d lines, got %zu", OBSERVORE_DIGEST_MAX_LINES, n);
    CHECK(strstr(body, "+4 more") != NULL, "the remainder is stated: %s", body);
    CHECK(strstr(title, "10 findings") != NULL,
          "the title counts everything, not just what fitted: %s", title);
    CHECK(strlen(body) < sizeof(body), "body stays inside its buffer");

    /* An empty digest is not a message. */
    CHECK(observore_digest_build(e, 0, NULL, title, sizeof(title),
                                 body, sizeof(body)) == 0,
          "nothing to report produces nothing");

    /* The whole point of the cap: the largest digest still has to fit the
     * tightest provider's request body after escaping. */
    observore_notify_request_t req;
    n = observore_digest_build(many, 10, "alert",
                               title, sizeof(title), body, sizeof(body));
    CHECK(observore_notify_build(OBSERVORE_PROVIDER_PUSHOVER,
                                 "https://api.pushover.net/1/messages.json",
                                 "abcdefghijklmnopqrstuvwxyz1234",
                                 "uvwxyz1234abcdefghijklmnopqrst",
                                 title, body, OBSERVORE_URGENCY_URGENT, &req),
          "a full digest must still build a Pushover request");
    CHECK(strstr(req.body, "%0A") != NULL,
          "newlines survive form encoding");
    CHECK(req.body[sizeof(req.body) - 1] == '\0', "request body is terminated");
    CHECK(strstr(req.body, "priority=") != NULL,
          "the priority field is not truncated away by a long digest: %zu bytes",
          strlen(req.body));
}

int main(void)
{
    test_oui_lookup();
    test_json_builder();
    test_notify_providers();
    test_vendor_lookup();
    test_vendor_labelling();
    test_name_capture();
    test_adv_parsing();
    test_ble_signatures();
    test_random_address();
    test_random_signals_disagree();
    test_ssid();
    test_follower();
    test_a_crowd_is_not_an_emergency();
    test_something_that_came_with_you();
    test_a_journey_must_change_the_room();
    test_the_battery_curve();
    test_bcd();
    test_the_watch_hands_point_the_right_way();
    test_the_dial_wanders_without_leaving_the_glass();
    test_the_census_earns_membership_over_days();
    test_a_crowd_cannot_hide_a_finding();
    test_scoring();
    test_rssi_floor();
    test_table_pressure();
    test_snapshot_order();
    test_wifi_remote_id();
    test_mute();
    test_fingerprint();
    test_fingerprint_safety();
    test_baseline_follower();
    test_ignoring_takes_it_off_the_screen();
    test_fast_pair_split();
    test_fixtures_are_not_followers();
    test_hunter_gear();
    test_squachmesh();
    test_baseline_does_not_blind();
    test_a_baseline_may_not_silence_a_kind();
    test_mute_rule_retires_when_it_covers_a_population();
    test_name_rule_matches_ble();
    test_mac_parsing();

    test_wps();
    test_edge_of_range_noise();
    test_rotation_continuity();
    test_webhook_and_telegram();
    test_heapwatch();
    test_version();
    test_name_keywords();
    test_digest();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
