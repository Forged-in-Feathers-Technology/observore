/* Host-side tests for the parts of Argus that do not need a radio:
 * advert parsing, signature matching, scoring and the follower heuristic.
 *
 *     make -C test && test/run
 */

#include <stdio.h>
#include <string.h>

#include "argus_detect.h"
#include "argus_mute.h"
#include "argus_track.h"

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
    const argus_oui_t *hit = argus_oui_lookup(flock);
    CHECK(hit != NULL, "Flock Safety OUI not found");
    CHECK(hit && hit->cls == ARGUS_CLASS_ALPR, "Flock should classify as ALPR");

    /* 00:25:DF is Axon Enterprise. */
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0xAA, 0xBB, 0xCC};
    hit = argus_oui_lookup(axon);
    CHECK(hit != NULL, "Axon OUI not found");
    CHECK(hit && hit->cls == ARGUS_CLASS_BODYCAM, "Axon should classify as bodycam");

    /* A locally-administered address must never match a vendor prefix. */
    const uint8_t random_mac[6] = {0xB6, 0x1E, 0x52, 0x01, 0x02, 0x03};
    CHECK(argus_mac_is_random(random_mac), "0xB6 has the LAA bit set");
    CHECK(argus_oui_lookup(random_mac) == NULL,
          "randomised MAC must not resolve to a vendor");

    const uint8_t nobody[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    CHECK(argus_oui_lookup(nobody) == NULL, "unassigned prefix must miss");
}

static void test_vendor_lookup(void)
{
    banner("benign vendor lookup");

    /* A4:B1:97 is Apple in the IEEE MA-L registry. */
    const uint8_t apple[6] = {0xA4, 0xB1, 0x97, 0x01, 0x02, 0x03};
    const char *v = argus_vendor_lookup(apple);
    CHECK(v != NULL && strcmp(v, "Apple") == 0, "expected Apple, got '%s'",
          v ? v : "(null)");

    /* 90:41:B2 is Ubiquiti -- observed live on real access points. */
    const uint8_t ubnt[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    v = argus_vendor_lookup(ubnt);
    CHECK(v != NULL && strcmp(v, "Ubiquiti") == 0, "expected Ubiquiti, got '%s'",
          v ? v : "(null)");

    /* A randomised address carries no vendor, and a virtual BSSID with the
     * locally-administered bit set must not be attributed to whoever happens
     * to own the matching universal prefix.  9A:41:B2 is the guest-SSID BSSID
     * of a 90:41:B2 Ubiquiti radio -- observed live. */
    const uint8_t virt[6] = {0x9A, 0x41, 0xB2, 0x01, 0x02, 0x03};
    CHECK(argus_mac_is_random(virt), "0x9A has the LAA bit set");
    CHECK(argus_vendor_lookup(virt) == NULL,
          "a locally-administered BSSID must not resolve to a vendor");

    /* An unassigned prefix misses cleanly rather than returning rubbish. */
    const uint8_t nobody[6] = {0x28, 0x1F, 0x7D, 0x01, 0x02, 0x03};
    CHECK(argus_vendor_lookup(nobody) == NULL, "unassigned prefix must miss");
    CHECK(argus_vendor_lookup(NULL) == NULL, "NULL must be handled");

    /* The benign table must never classify or score.  This is the whole
     * reason it is a separate table. */
    argus_event_t ev;
    uint8_t boring[] = {0x02, 0x01, 0x06};
    argus_observation_t obs = {.mac = apple, .src = ARGUS_SRC_BLE, .rssi = -50,
                               .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!argus_classify(&obs, &ev),
          "a benign vendor must not produce a detection");

    /* And a threat prefix must still win: Ring is in the threat table, so it
     * classifies even though Amazon-family kit is otherwise benign. */
    const uint8_t ring[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03};
    obs.mac = ring;
    CHECK(argus_classify(&obs, &ev), "Ring must still classify");
    CHECK(ev.cls == ARGUS_CLASS_CAMERA, "Ring should be a camera");
    CHECK(argus_vendor_lookup(ring) == NULL,
          "a prefix claimed as a threat must not also appear as benign");
}

static void test_vendor_labelling(void)
{
    banner("vendor labelling in the tracker");

    argus_mute_init();
    argus_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    /* An unclassified device still gets a vendor name, which is what makes it
     * recognisable enough to ignore. */
    const uint8_t apple[6] = {0xA4, 0xB1, 0x97, 0x0A, 0x0B, 0x0C};
    argus_observation_t obs = {.mac = apple, .src = ARGUS_SRC_BLE, .rssi = -50,
                               .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!argus_track_observe(&obs, SECS(0)), "must not be reportable");

    argus_event_t nearby[8];
    size_t n = argus_track_nearby(nearby, 8, SECS(0));
    CHECK(n == 1, "expected 1 nearby device, got %zu", n);
    CHECK(n == 1 && nearby[0].vendor && strcmp(nearby[0].vendor, "Apple") == 0,
          "nearby device should be labelled Apple");

    argus_status_t st;
    argus_track_status(&st, SECS(0));
    CHECK(st.score == 0, "labelling must not score, got %u", st.score);
    CHECK(st.device_count == 0, "labelling must not create a detection");

    /* A follower gets the vendor in its label, which is the difference between
     * "something is following you" and "a Samsung is following you". */
    argus_track_observe(&obs, SECS(10));
    argus_track_observe(&obs, SECS(20));
    CHECK(argus_track_observe(&obs, SECS(400)), "follower not promoted");
    argus_event_t snap[4];
    n = argus_track_snapshot(snap, 4, SECS(400));
    CHECK(n == 1 && strstr(snap[0].label, "Apple") != NULL,
          "follower label should name the vendor, got '%s'",
          n ? snap[0].label : "");

    /* Nearby must exclude anything already classified. */
    n = argus_track_nearby(nearby, 8, SECS(400));
    CHECK(n == 0, "a classified device must leave the nearby list, got %zu", n);
}

static void test_name_capture(void)
{
    banner("name and ssid capture");

    argus_mute_init();
    argus_track_init();

    /* An unclassified BLE device that broadcasts a name must carry it, even
     * though nothing about it classified. */
    const uint8_t mac[6] = {0xA4, 0xB1, 0x97, 0x01, 0x02, 0x03};
    uint8_t named[] = {0x02, 0x01, 0x06,
                       0x06, 0x09, 'S', 'h', 'e', 'l', 'f'};
    argus_observation_t obs = {.mac = mac, .src = ARGUS_SRC_BLE, .rssi = -50,
                               .adv = named, .adv_len = sizeof(named)};
    CHECK(!argus_track_observe(&obs, SECS(0)), "must not classify");

    argus_event_t nearby[8];
    size_t n = argus_track_nearby(nearby, 8, SECS(0));
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "name not captured, got '%s'", n ? nearby[0].detail : "");

    /* BLE names often arrive in a scan response, several sightings after the
     * device first appears.  A later nameless advert from the same address
     * must not wipe the name we already learned. */
    uint8_t nameless[] = {0x02, 0x01, 0x06};
    obs.adv = nameless;
    obs.adv_len = sizeof(nameless);
    argus_track_observe(&obs, SECS(5));
    n = argus_track_nearby(nearby, 8, SECS(5));
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "a nameless advert erased the learned name, got '%s'",
          n ? nearby[0].detail : "");

    /* And the reverse: a name arriving late is picked up. */
    argus_track_init();
    obs.adv = nameless;
    obs.adv_len = sizeof(nameless);
    argus_track_observe(&obs, SECS(0));
    n = argus_track_nearby(nearby, 8, SECS(0));
    CHECK(n == 1 && nearby[0].detail[0] == '\0', "should start nameless");
    obs.adv = named;
    obs.adv_len = sizeof(named);
    argus_track_observe(&obs, SECS(1));
    n = argus_track_nearby(nearby, 8, SECS(1));
    CHECK(n == 1 && strcmp(nearby[0].detail, "Shelf") == 0,
          "a late name was not learned, got '%s'", n ? nearby[0].detail : "");

    /* Wi-Fi carries the SSID the same way. */
    argus_track_init();
    const uint8_t ap[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    argus_observation_t wifi = {.mac = ap, .src = ARGUS_SRC_WIFI_SCAN,
                                .rssi = -50, .ssid = "42-Guest"};
    argus_track_observe(&wifi, SECS(0));
    n = argus_track_nearby(nearby, 8, SECS(0));
    CHECK(n == 1 && strcmp(nearby[0].detail, "42-Guest") == 0,
          "ssid not captured, got '%s'", n ? nearby[0].detail : "");
    CHECK(n == 1 && nearby[0].vendor && strcmp(nearby[0].vendor, "Ubiquiti") == 0,
          "vendor should still resolve alongside the ssid");

    /* A hidden AP reports an empty SSID and must simply stay nameless. */
    argus_track_init();
    wifi.ssid = "";
    argus_track_observe(&wifi, SECS(0));
    n = argus_track_nearby(nearby, 8, SECS(0));
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
    CHECK(argus_adv_name(adv, sizeof(adv), name, sizeof(name)), "name not parsed");
    CHECK(strcmp(name, "AxonX") == 0, "got name '%s'", name);

    /* A field claiming more bytes than remain must not read past the end. */
    const uint8_t truncated[] = {0x02, 0x01, 0x06, 0x20, 0x09, 'A'};
    CHECK(!argus_adv_name(truncated, sizeof(truncated), name, sizeof(name)),
          "over-long field must be rejected, not parsed");

    /* Zero-length field terminates the walk rather than looping forever. */
    const uint8_t padded[] = {0x02, 0x01, 0x06, 0x00, 0x00, 0x00};
    size_t len = 0;
    CHECK(argus_adv_field(padded, sizeof(padded), 0x09, &len) == NULL,
          "padding must not yield a field");

    /* Non-printable bytes in a name are sanitised, not passed through. */
    const uint8_t nasty[] = {0x05, 0x09, 'a', 0x00, 0x1B, 'b'};
    CHECK(argus_adv_name(nasty, sizeof(nasty), name, sizeof(name)), "name missing");
    CHECK(strcmp(name, "a..b") == 0, "unsanitised name '%s'", name);
}

static bool classify_ble(const uint8_t *mac, const uint8_t *adv, size_t adv_len,
                         argus_event_t *out)
{
    argus_observation_t obs = {
        .mac = mac, .src = ARGUS_SRC_BLE, .rssi = -50,
        .adv = adv, .adv_len = adv_len,
    };
    return argus_classify(&obs, out);
}

static void test_ble_signatures(void)
{
    banner("ble signatures");

    const uint8_t mac[6] = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55}; /* random */
    argus_event_t ev;

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
    CHECK(ev.cls == ARGUS_CLASS_TRACKER, "Find My should be a tracker");
    CHECK(ev.evidence == ARGUS_EVIDENCE_MFG_DATA, "expected mfg-data evidence");

    /* An Apple advert of a different payload type is ordinary Apple traffic
     * and must not be reported -- every iPhone in the room emits these. */
    uint8_t nearby[10] = {0x09, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0, 0, 0, 0};
    CHECK(!classify_ble(mac, nearby, sizeof(nearby), &ev),
          "Apple Nearby Info must not be flagged as a tracker");

    /* OpenDroneID: service data under 0xFFFA opening with app code 0x0D. */
    uint8_t odid[] = {0x06, 0x16, 0xFA, 0xFF, 0x0D, 0x00, 0x01};
    CHECK(classify_ble(mac, odid, sizeof(odid), &ev), "Remote ID not detected");
    CHECK(ev.cls == ARGUS_CLASS_DRONE, "Remote ID should be a drone");

    /* Same UUID, different ASTM application -- not a drone. */
    uint8_t not_odid[] = {0x06, 0x16, 0xFA, 0xFF, 0x01, 0x00, 0x01};
    CHECK(!classify_ble(mac, not_odid, sizeof(not_odid), &ev),
          "non-ODID ASTM service data must not be flagged");

    /* Tile advertises service UUID 0xFEED. */
    uint8_t tile[] = {0x03, 0x03, 0xED, 0xFE};
    CHECK(classify_ble(mac, tile, sizeof(tile), &ev), "Tile not detected");
    CHECK(ev.cls == ARGUS_CLASS_TRACKER, "Tile should be a tracker");

    /* Name keyword fallback. */
    uint8_t named[] = {0x09, 0x09, 'F', 'l', 'o', 'c', 'k', '-', '1', '2'};
    CHECK(classify_ble(mac, named, sizeof(named), &ev), "name keyword missed");
    CHECK(ev.cls == ARGUS_CLASS_ALPR, "Flock name should be ALPR");
    CHECK(ev.evidence == ARGUS_EVIDENCE_NAME, "expected name evidence");

    /* A bare Samsung company ID must NOT be reported.  Every Samsung phone,
     * watch and earbud advertises 0x0075; matching the vendor alone turns a
     * crowded room into a wall of phantom trackers.  Observed live. */
    uint8_t samsung[] = {0x05, 0xFF, 0x75, 0x00, 0x42, 0x01, 0x00};
    CHECK(!classify_ble(mac, samsung, sizeof(samsung), &ev),
          "bare Samsung company ID must not be flagged as a tracker");

    /* The actual SmartTag signature is its 0xFD5A service data. */
    uint8_t smarttag[] = {0x05, 0x16, 0x5A, 0xFD, 0x01, 0x02};
    CHECK(classify_ble(mac, smarttag, sizeof(smarttag), &ev), "SmartTag missed");
    CHECK(ev.cls == ARGUS_CLASS_TRACKER, "SmartTag should be a tracker");

    /* Nothing at all. */
    uint8_t boring[] = {0x02, 0x01, 0x06};
    CHECK(!classify_ble(mac, boring, sizeof(boring), &ev),
          "a bare flags advert must not match");

    /* A payload signature must win over a vendor prefix, because trackers
     * rotate MACs and the prefix is the weaker evidence. */
    const uint8_t ring_mac[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03}; /* Ring */
    CHECK(classify_ble(ring_mac, findmy, sizeof(findmy), &ev), "no match");
    CHECK(ev.cls == ARGUS_CLASS_TRACKER,
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
    argus_observation_t obs = {
        .mac = looks_like_ring, .src = ARGUS_SRC_BLE, .rssi = -50,
        .addr_random = true, .adv = boring, .adv_len = sizeof(boring),
    };
    argus_event_t ev;
    CHECK(!argus_classify(&obs, &ev),
          "a random address must not resolve to a vendor prefix");

    /* The same address reported as public does resolve. */
    obs.addr_random = false;
    CHECK(argus_classify(&obs, &ev), "public address should match the OUI");
    CHECK(ev.cls == ARGUS_CLASS_CAMERA, "should be a camera");
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
    argus_observation_t a = {.mac = laa_only, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .addr_random = false,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(argus_obs_is_random(&a), "LAA bit alone must count as random");

    /* Controller says random, LAA bit says universal (20:7C:3A, observed). */
    const uint8_t type_only[6] = {0x20, 0x7C, 0x3A, 0x7D, 0x1A, 0x20};
    argus_observation_t b = {.mac = type_only, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .addr_random = true,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(argus_obs_is_random(&b), "the transport flag alone must count too");
    CHECK(!argus_mac_is_random(type_only),
          "this address's LAA bit is clear -- that is the point of the test");

    /* A genuinely public address is still usable. */
    const uint8_t real[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    argus_observation_t c = {.mac = real, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .addr_random = false,
                             .adv = boring, .adv_len = sizeof(boring)};
    CHECK(!argus_obs_is_random(&c), "a public address must stay usable");

    /* The flag reaches the tracker, so the UI can say "random MAC" rather
     * than "unknown vendor" -- different facts. */
    argus_mute_init();
    argus_track_init();
    argus_track_observe(&a, SECS(0));
    argus_event_t nearby[4];
    size_t n = argus_track_nearby(nearby, 4, SECS(0));
    CHECK(n == 1 && nearby[0].addr_random,
          "tracker should record the address as random");
    CHECK(n == 1 && nearby[0].vendor == NULL,
          "a random address must not carry a vendor");

    CHECK(argus_obs_is_random(NULL), "NULL must be treated as unknowable");
}

static void test_ssid(void)
{
    banner("ssid keywords");

    char label[24];
    CHECK(argus_ssid_is_suspicious("FlockSafety_Falcon", label, sizeof(label)),
          "Flock SSID missed");
    CHECK(argus_ssid_is_suspicious("lobby-CCTV-2", label, sizeof(label)),
          "CCTV SSID missed");
    CHECK(!argus_ssid_is_suspicious("Starbucks WiFi", label, sizeof(label)),
          "ordinary SSID must not match");
    CHECK(!argus_ssid_is_suspicious("", label, sizeof(label)), "empty SSID");
}

static void test_follower(void)
{
    banner("follower heuristic");

    argus_track_init();
    const uint8_t mac[6] = {0x4A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    argus_observation_t obs = {
        .mac = mac, .src = ARGUS_SRC_BLE, .rssi = -60,
        .adv = boring, .adv_len = sizeof(boring),
    };

    /* Three sightings inside a minute are a device sitting nearby, not one
     * following you -- the span requirement is what separates them. */
    CHECK(!argus_track_observe(&obs, SECS(0)), "first sighting is not a hit");
    CHECK(!argus_track_observe(&obs, SECS(10)), "second sighting is not a hit");
    CHECK(!argus_track_observe(&obs, SECS(20)), "3 hits in 20s must not trip");

    /* Same address still around five minutes later: that is a follower. */
    CHECK(argus_track_observe(&obs, SECS(301)), "follower not promoted");

    argus_status_t st;
    argus_track_status(&st, SECS(301));
    CHECK(st.class_counts[ARGUS_CLASS_FOLLOWER] == 1, "follower not counted");
    CHECK(st.score == argus_class_points(ARGUS_CLASS_FOLLOWER),
          "follower should score %u, got %u",
          argus_class_points(ARGUS_CLASS_FOLLOWER), st.score);
}

static void test_scoring(void)
{
    banner("scoring, cooldown and decay");

    argus_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    argus_observation_t obs = {
        .mac = axon, .src = ARGUS_SRC_BLE, .rssi = -40,
        .adv = boring, .adv_len = sizeof(boring),
    };

    CHECK(argus_track_observe(&obs, SECS(0)), "bodycam not reported");
    argus_status_t st;
    argus_track_status(&st, SECS(0));
    CHECK(st.score == 5, "bodycam should score 5, got %u", st.score);
    CHECK(st.level == ARGUS_LEVEL_CAUTION, "5 points is caution");

    /* A chatty beacon must not run the score away: inside the cooldown the
     * repeat sightings are tracked but not scored. */
    for (int i = 1; i < 50; i++) {
        argus_track_observe(&obs, SECS(i));
    }
    argus_track_status(&st, SECS(50));
    CHECK(st.score == 5, "cooldown breached: score ran to %u", st.score);

    /* Decay is continuous and applied lazily, so the score at any instant does
     * not depend on how often tick() happened to run.  By t=200 the score has
     * shed three points, and the cooldown has expired so the sighting scores
     * another five: 5 - 3 + 5 = 7. */
    argus_track_observe(&obs, SECS(200));
    argus_track_status(&st, SECS(200));
    CHECK(st.score == 7, "expected 7 at t=200, got %u", st.score);
    CHECK(st.level == ARGUS_LEVEL_ALERT, "7 points is alert");

    /* Three more quiet minutes, three more points shed. */
    argus_track_tick(SECS(380));
    argus_track_status(&st, SECS(380));
    CHECK(st.score == 4, "expected 4 after further decay, got %u", st.score);
    CHECK(st.level == ARGUS_LEVEL_CAUTION, "4 points is caution");

    /* Decay must floor at zero and not leave debt that eats a later hit. */
    argus_track_tick(SECS(10000));
    argus_track_status(&st, SECS(10000));
    CHECK(st.score == 0, "score should floor at 0, got %u", st.score);
    argus_track_observe(&obs, SECS(10001));
    argus_track_status(&st, SECS(10002));
    CHECK(st.score == 5, "a fresh hit after a long quiet must survive, got %u",
          st.score);

    /* And the reading must not depend on tick() having been called at all. */
    argus_track_init();
    argus_track_observe(&obs, SECS(0));
    argus_track_status(&st, SECS(120));
    CHECK(st.score == 3, "lazy decay without tick(): expected 3, got %u",
          st.score);
}

static void test_rssi_floor(void)
{
    banner("rssi floor");

    argus_track_init();
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    uint8_t boring[] = {0x02, 0x01, 0x06};
    argus_observation_t obs = {
        .mac = axon, .src = ARGUS_SRC_BLE, .rssi = -99,
        .adv = boring, .adv_len = sizeof(boring),
    };
    CHECK(!argus_track_observe(&obs, SECS(0)), "-99 dBm must be dropped");

    argus_status_t st;
    argus_track_status(&st, SECS(0));
    CHECK(st.score == 0, "a dropped sighting must not score");
}

static void test_table_pressure(void)
{
    banner("device table eviction");

    argus_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    /* One confirmed bodycam, then far more unclassified noise than the table
     * can hold.  The bodycam must survive -- evicting it to make room for a
     * passing phone is the failure this guards against. */
    const uint8_t axon[6] = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03};
    argus_observation_t cam = {
        .mac = axon, .src = ARGUS_SRC_BLE, .rssi = -40,
        .adv = boring, .adv_len = sizeof(boring),
    };
    argus_track_observe(&cam, SECS(0));

    for (int i = 0; i < ARGUS_MAX_DEVICES * 2; i++) {
        uint8_t noise[6] = {0x4A, 0x00, 0x00,
                            (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i};
        argus_observation_t obs = {
            .mac = noise, .src = ARGUS_SRC_BLE, .rssi = -70,
            .adv = boring, .adv_len = sizeof(boring),
        };
        argus_track_observe(&obs, SECS(1 + i));
    }

    argus_event_t snap[ARGUS_MAX_DEVICES];
    size_t n = argus_track_snapshot(snap, ARGUS_MAX_DEVICES, SECS(10000));
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

    argus_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};
    const uint8_t older[6] = {0x00, 0x25, 0xDF, 0x00, 0x00, 0x01};  /* Axon */
    const uint8_t newer[6] = {0xB4, 0x1E, 0x52, 0x00, 0x00, 0x02};  /* Flock */

    argus_observation_t a = {.mac = older, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .adv = boring, .adv_len = sizeof(boring)};
    argus_observation_t b = {.mac = newer, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .adv = boring, .adv_len = sizeof(boring)};
    argus_track_observe(&a, SECS(10));
    argus_track_observe(&b, SECS(20));

    argus_event_t snap[8];
    size_t n = argus_track_snapshot(snap, 8, SECS(30));
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
    argus_observation_t obs = {
        .mac = generic, .src = ARGUS_SRC_WIFI_SNIFF, .rssi = -55,
        .channel = 6, .ssid = "DroneOps", .remote_id = true,
    };
    argus_event_t ev;
    CHECK(argus_classify(&obs, &ev), "remote id frame not classified");
    CHECK(ev.cls == ARGUS_CLASS_DRONE, "should be a drone");
    CHECK(strcmp(ev.detail, "DroneOps") == 0, "ssid not carried, got '%s'",
          ev.detail);

    /* Remote ID outranks a vendor prefix that would otherwise win. */
    const uint8_t ring[6] = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03};
    obs.mac = ring;
    CHECK(argus_classify(&obs, &ev), "no match");
    CHECK(ev.cls == ARGUS_CLASS_DRONE, "remote id must outrank the OUI");

    /* Without the flag it falls back to the ordinary path. */
    obs.remote_id = false;
    CHECK(argus_classify(&obs, &ev), "ring OUI should still match");
    CHECK(ev.cls == ARGUS_CLASS_CAMERA, "should fall back to camera");
}

static void test_mute(void)
{
    banner("mute rules");

    argus_mute_init();
    argus_track_init();
    uint8_t boring[] = {0x02, 0x01, 0x06};

    const uint8_t ring[6]  = {0x54, 0xE0, 0x19, 0x01, 0x02, 0x03}; /* camera  */
    const uint8_t ring2[6] = {0x54, 0xE0, 0x19, 0x09, 0x09, 0x09}; /* same OUI */
    const uint8_t axon[6]  = {0x00, 0x25, 0xDF, 0x01, 0x02, 0x03}; /* bodycam */

    argus_observation_t o_ring  = {.mac = ring,  .src = ARGUS_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};
    argus_observation_t o_ring2 = {.mac = ring2, .src = ARGUS_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};
    argus_observation_t o_axon  = {.mac = axon,  .src = ARGUS_SRC_BLE, .rssi = -50,
                                   .adv = boring, .adv_len = sizeof(boring)};

    /* Baseline: both report. */
    CHECK(argus_track_observe(&o_ring, SECS(0)), "ring should report");
    CHECK(argus_track_observe(&o_axon, SECS(0)), "axon should report");

    /* Mute one exact address.  Its neighbour on the same OUI must survive. */
    argus_track_init();
    argus_mute_rule_t r = {.kind = ARGUS_MUTE_MAC};
    memcpy(r.mac, ring, 6);
    CHECK(argus_mute_add(&r) == ESP_OK, "add mac rule");
    CHECK(!argus_track_observe(&o_ring, SECS(0)), "muted mac must be suppressed");
    CHECK(argus_track_observe(&o_ring2, SECS(0)), "a different mac must survive");

    argus_status_t st;
    argus_track_status(&st, SECS(0));
    CHECK(st.score == argus_class_points(ARGUS_CLASS_CAMERA),
          "only the unmuted device should have scored, got %u", st.score);

    /* Muting the vendor prefix takes both. */
    argus_mute_clear();
    argus_track_init();
    r = (argus_mute_rule_t){.kind = ARGUS_MUTE_OUI};
    memcpy(r.mac, ring, 3);
    CHECK(argus_mute_add(&r) == ESP_OK, "add oui rule");
    CHECK(!argus_track_observe(&o_ring, SECS(0)), "oui rule should suppress");
    CHECK(!argus_track_observe(&o_ring2, SECS(0)), "oui rule should suppress");
    CHECK(argus_track_observe(&o_axon, SECS(0)), "a different vendor must survive");

    /* Muting a class takes the class and nothing else. */
    argus_mute_clear();
    argus_track_init();
    r = (argus_mute_rule_t){.kind = ARGUS_MUTE_CLASS, .cls = ARGUS_CLASS_CAMERA};
    CHECK(argus_mute_add(&r) == ESP_OK, "add class rule");
    CHECK(!argus_track_observe(&o_ring, SECS(0)), "camera class muted");
    CHECK(argus_track_observe(&o_axon, SECS(0)), "bodycam must still report");

    /* A class rule must not swallow unclassified traffic, or muting cameras
     * would quietly disable the follower heuristic too. */
    const uint8_t stranger[6] = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55};
    argus_observation_t o_unknown = {.mac = stranger, .src = ARGUS_SRC_BLE,
                                     .rssi = -60, .addr_random = true,
                                     .adv = boring, .adv_len = sizeof(boring)};
    argus_track_observe(&o_unknown, SECS(0));
    argus_track_observe(&o_unknown, SECS(10));
    argus_track_observe(&o_unknown, SECS(20));
    CHECK(argus_track_observe(&o_unknown, SECS(400)),
          "follower heuristic must survive a class mute");

    /* SSID substring, case-insensitively. */
    argus_mute_clear();
    argus_track_init();
    r = (argus_mute_rule_t){.kind = ARGUS_MUTE_SSID};
    snprintf(r.ssid, sizeof(r.ssid), "lobby");
    CHECK(argus_mute_add(&r) == ESP_OK, "add ssid rule");
    const uint8_t ap[6] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x03};
    argus_observation_t o_ap = {.mac = ap, .src = ARGUS_SRC_WIFI_SCAN,
                                .rssi = -50, .ssid = "Lobby-CCTV-2"};
    CHECK(!argus_track_observe(&o_ap, SECS(0)), "ssid rule should suppress");
    o_ap.ssid = "Garage-CCTV-2";
    CHECK(argus_track_observe(&o_ap, SECS(0)), "a different ssid must survive");

    /* List hygiene. */
    argus_mute_clear();
    r = (argus_mute_rule_t){.kind = ARGUS_MUTE_CLASS, .cls = ARGUS_CLASS_CAMERA};
    CHECK(argus_mute_add(&r) == ESP_OK, "first add");
    CHECK(argus_mute_add(&r) == ESP_OK, "duplicate add should succeed");
    CHECK(argus_mute_count() == 1, "duplicates must not accumulate, got %zu",
          argus_mute_count());

    /* Rules that would match everything are refused. */
    argus_mute_rule_t empty_ssid = {.kind = ARGUS_MUTE_SSID};
    CHECK(argus_mute_add(&empty_ssid) == ESP_ERR_INVALID_ARG,
          "an empty ssid rule matches everything and must be refused");
    argus_mute_rule_t bad_class = {.kind = ARGUS_MUTE_CLASS,
                                   .cls = ARGUS_CLASS_UNKNOWN};
    CHECK(argus_mute_add(&bad_class) == ESP_ERR_INVALID_ARG,
          "muting the unknown class must be refused");

    CHECK(argus_mute_remove(99) == ESP_ERR_NOT_FOUND, "out of range remove");
    CHECK(argus_mute_remove(0) == ESP_OK, "in range remove");
    CHECK(argus_mute_count() == 0, "list should be empty");

    argus_mute_clear();
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

    uint32_t fa = argus_fingerprint(a, sizeof(a));
    uint32_t fb = argus_fingerprint(b, sizeof(b));
    CHECK(fa != 0, "fingerprint should not be zero");
    CHECK(fa == fb, "a rotated payload must not change the fingerprint");

    /* A different company ID is a different kind of device. */
    uint8_t c[31];
    memcpy(c, a, sizeof(c));
    c[2] = 0x75; c[3] = 0x00;             /* Samsung instead of Apple */
    CHECK(argus_fingerprint(c, sizeof(c)) != fa,
          "a different company ID must change the fingerprint");

    /* So is a different name. */
    uint8_t n1[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'T', 'V', '-', '1'};
    uint8_t n2[] = {0x02, 0x01, 0x06, 0x05, 0x09, 'T', 'V', '-', '2'};
    CHECK(argus_fingerprint(n1, sizeof(n1)) != argus_fingerprint(n2, sizeof(n2)),
          "different names must fingerprint differently");

    /* Nothing to hash yields zero, which means "no fingerprint". */
    CHECK(argus_fingerprint(NULL, 0) == 0, "NULL advert");
    uint8_t empty[] = {0x00, 0x00};
    CHECK(argus_fingerprint(empty, sizeof(empty)) == 0, "padding-only advert");
}

static void test_fingerprint_safety(void)
{
    banner("fingerprint rules cannot silence a threat");

    argus_mute_init();
    argus_track_init();

    /* An AirTag-shaped advert on a rotating address. */
    uint8_t findmy[31] = {0};
    findmy[0] = 0x1E; findmy[1] = 0xFF; findmy[2] = 0x4C; findmy[3] = 0x00;
    findmy[4] = 0x12; findmy[5] = 0x19; findmy[6] = 0x10;

    const uint8_t mine[6]  = {0x4A, 0x11, 0x22, 0x33, 0x44, 0x55};
    argus_observation_t obs = {.mac = mine, .src = ARGUS_SRC_BLE, .rssi = -50,
                               .addr_random = true,
                               .adv = findmy, .adv_len = sizeof(findmy)};

    argus_mute_rule_t fp = {.kind = ARGUS_MUTE_FINGERPRINT,
                            .fingerprint = argus_fingerprint(findmy,
                                                             sizeof(findmy))};
    CHECK(argus_mute_add(&fp) == ESP_OK, "fingerprint rule should be accepted");

    /* Muting your own tracker by fingerprint would silence a stranger's too,
     * so the rule must not apply to a tracker at all. */
    CHECK(argus_track_observe(&obs, SECS(0)),
          "a tracker must still report despite a matching fingerprint rule");
    argus_status_t st;
    argus_track_status(&st, SECS(0));
    CHECK(st.class_counts[ARGUS_CLASS_TRACKER] == 1, "tracker should be counted");

    /* The same rule does work on an unprotected class. */
    CHECK(argus_mute_class_is_protected(ARGUS_CLASS_TRACKER), "tracker protected");
    CHECK(argus_mute_class_is_protected(ARGUS_CLASS_BODYCAM), "bodycam protected");
    CHECK(argus_mute_class_is_protected(ARGUS_CLASS_FOLLOWER),
          "follower protected -- it is the anti-stalking case");
    CHECK(!argus_mute_class_is_protected(ARGUS_CLASS_CAMERA),
          "cameras are street furniture and may be muted wholesale");

    argus_mute_clear();
    argus_track_init();
    uint8_t plain[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xAA, 0xBB};
    argus_observation_t un = {.mac = mine, .src = ARGUS_SRC_BLE, .rssi = -50,
                              .addr_random = true,
                              .adv = plain, .adv_len = sizeof(plain)};
    argus_mute_rule_t fp2 = {.kind = ARGUS_MUTE_FINGERPRINT,
                             .fingerprint = argus_fingerprint(plain,
                                                              sizeof(plain))};
    CHECK(argus_mute_add(&fp2) == ESP_OK, "add");

    /* Unclassified traffic IS suppressed, including across a MAC change --
     * that is the whole point of fingerprinting. */
    argus_track_observe(&un, SECS(0));
    const uint8_t rotated[6] = {0x4A, 0x99, 0x88, 0x77, 0x66, 0x55};
    argus_observation_t un2 = un;
    un2.mac = rotated;
    argus_track_observe(&un2, SECS(10));
    argus_event_t nearby[8];
    CHECK(argus_track_nearby(nearby, 8, SECS(10)) == 0,
          "a fingerprint rule must suppress the device under any address");

    /* A zero fingerprint rule would match every nameless advert. */
    argus_mute_rule_t zero = {.kind = ARGUS_MUTE_FINGERPRINT, .fingerprint = 0};
    CHECK(argus_mute_add(&zero) == ESP_ERR_INVALID_ARG,
          "a zero fingerprint rule must be refused");
}

static void test_name_rule_matches_ble(void)
{
    banner("name rules across a MAC rotation");

    argus_mute_init();
    argus_track_init();

    /* A named BLE device that changes address must stay muted -- this is what
     * a MAC rule could never do. */
    uint8_t named[] = {0x02, 0x01, 0x06,
                       0x08, 0x09, 'E', 'n', 'c', 'h', 'a', 'r', 'g'};
    argus_mute_rule_t r = {.kind = ARGUS_MUTE_NAME};
    snprintf(r.ssid, sizeof(r.ssid), "Encharg");
    CHECK(argus_mute_add(&r) == ESP_OK, "add name rule");

    const uint8_t mac1[6] = {0x4A, 0x01, 0x02, 0x03, 0x04, 0x05};
    const uint8_t mac2[6] = {0x4A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    argus_observation_t o = {.mac = mac1, .src = ARGUS_SRC_BLE, .rssi = -50,
                             .addr_random = true,
                             .adv = named, .adv_len = sizeof(named)};
    CHECK(!argus_track_observe(&o, SECS(0)), "named device should be muted");
    o.mac = mac2;
    CHECK(!argus_track_observe(&o, SECS(60)),
          "still muted after the address rotates");

    argus_event_t nearby[8];
    CHECK(argus_track_nearby(nearby, 8, SECS(60)) == 0,
          "neither address should be tracked");

    /* And it still works for Wi-Fi SSIDs, which is where it started. */
    argus_track_init();
    const uint8_t ap[6] = {0x90, 0x41, 0xB2, 0x01, 0x02, 0x03};
    argus_observation_t w = {.mac = ap, .src = ARGUS_SRC_WIFI_SCAN,
                             .rssi = -50, .ssid = "Encharger-Guest"};
    CHECK(!argus_track_observe(&w, SECS(0)), "ssid should match the name rule");
}

static void test_mac_parsing(void)
{
    banner("mac parsing");

    uint8_t mac[6];
    CHECK(argus_mute_parse_mac("AA:BB:CC:DD:EE:FF", mac, 6), "colon form");
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF, "wrong bytes");
    CHECK(argus_mute_parse_mac("aabbccddeeff", mac, 6), "bare form");
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF, "wrong bytes");
    CHECK(argus_mute_parse_mac("AA-BB-CC", mac, 3), "dash form, 3 bytes");

    /* A typo must be an error, not a rule that silently matches the wrong
     * device.  Trailing rubbish is the dangerous case. */
    CHECK(!argus_mute_parse_mac("AA:BB:CC:DD:EE:FF:00", mac, 6), "too long");
    CHECK(!argus_mute_parse_mac("AA:BB:CC", mac, 6), "too short");
    CHECK(!argus_mute_parse_mac("ZZ:BB:CC:DD:EE:FF", mac, 6), "non-hex");
    CHECK(!argus_mute_parse_mac("", mac, 6), "empty");

    argus_class_t cls;
    CHECK(argus_mute_parse_class("bodycam", &cls), "class name");
    CHECK(cls == ARGUS_CLASS_BODYCAM, "wrong class");
    CHECK(!argus_mute_parse_class("nonsense", &cls), "unknown class");
    CHECK(!argus_mute_parse_class("unknown", &cls),
          "the unknown class must not be addressable by name");
}

int main(void)
{
    test_oui_lookup();
    test_vendor_lookup();
    test_vendor_labelling();
    test_name_capture();
    test_adv_parsing();
    test_ble_signatures();
    test_random_address();
    test_random_signals_disagree();
    test_ssid();
    test_follower();
    test_scoring();
    test_rssi_floor();
    test_table_pressure();
    test_snapshot_order();
    test_wifi_remote_id();
    test_mute();
    test_fingerprint();
    test_fingerprint_safety();
    test_name_rule_matches_ble();
    test_mac_parsing();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
