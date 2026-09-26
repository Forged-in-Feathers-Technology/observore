#include <string.h>

#include "observore_detect.h"
#include "observore_mute.h"
#include "observore_track.h"
#include <stdio.h>

#ifdef OBSERVORE_HOST_TEST
/* The host build exercises the matching and list logic without NVS. */
#include <stdio.h>
#define MUTE_LOCK()   do {} while (0)
#define MUTE_UNLOCK() do {} while (0)
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
static const char *TAG = "observore.mute";
static void mute_load(void) {}
static void mute_save(void) {}
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "observore_nvs.h"

static const char *TAG = "observore.mute";
static SemaphoreHandle_t s_lock;
#define MUTE_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define MUTE_UNLOCK() xSemaphoreGiveRecursive(s_lock)

#include "observore_nvs.h"
#endif

static observore_mute_rule_t s_rules[OBSERVORE_MUTE_MAX];
static size_t            s_count;

/* Per-rule reach, kept in RAM only: it describes what has happened since boot,
 * not what the rule is, and persisting it would make a rule that misbehaved
 * once look guilty forever. `last` is the address most recently suppressed, so
 * a change of address can be counted without storing every one. */
static struct {
    uint32_t suppressed;
    uint8_t  addresses;
    bool     disabled;
    uint8_t  last[OBSERVORE_MAC_LEN];
    bool     have_last;
} s_stat[OBSERVORE_MUTE_MAX];
static uint32_t          s_suppressed;

/* ------------------------------------------------------------------ */
/* Persistence                                                        */
/* ------------------------------------------------------------------ */

#ifndef OBSERVORE_HOST_TEST
static void mute_load(void)
{
    observore_nvs_item_t item = {.key = "mutes", .type = OBSERVORE_NVS_BLOB,
                                 .buf = s_rules, .len = sizeof(s_rules)};
    observore_nvs_read(&item, 1);

    if (!item.found) {
        return;
    }
    if (item.len % sizeof(observore_mute_rule_t) != 0) {
        /* A short or corrupt blob would otherwise be read as garbage rules
         * that silently suppress real detections. */
        ESP_LOGW(TAG, "discarding a stored mute list of %zu bytes, which is "
                      "not a whole number of rules", item.len);
        memset(s_rules, 0, sizeof(s_rules));
        s_count = 0;
        return;
    }
    s_count = item.len / sizeof(observore_mute_rule_t);
    if (s_count > OBSERVORE_MUTE_MAX) {
        s_count = OBSERVORE_MUTE_MAX;
    }
    ESP_LOGI(TAG, "loaded %zu mute rules", s_count);
}

static void mute_save(void)
{
    const observore_nvs_item_t item = {
        .key = "mutes", .type = OBSERVORE_NVS_BLOB, .buf = s_rules,
        .len = s_count * sizeof(observore_mute_rule_t)};
    observore_nvs_write(&item, 1);
}
#endif

void observore_mute_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    MUTE_LOCK();
    s_count = 0;
    s_suppressed = 0;
    memset(s_rules, 0, sizeof(s_rules));
    mute_load();
    MUTE_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Matching                                                           */
/* ------------------------------------------------------------------ */

bool observore_mute_class_is_protected(observore_class_t cls)
{
    /* Cameras and fleet telematics are deliberately unprotected: those are
     * street furniture, and muting a whole brand of them is the point. */
    return observore_class_desc(cls)->protected_cls;
}

/* Count what a rule has covered, and retire a fingerprint rule that has
 * clearly stopped describing a device. Called with the lock held. */
static void note_reach(size_t i, const uint8_t mac[OBSERVORE_MAC_LEN])
{
    if (i >= OBSERVORE_MUTE_MAX) {
        return;
    }
    if (s_stat[i].suppressed < UINT32_MAX) {
        s_stat[i].suppressed++;
    }
    if (!s_stat[i].have_last ||
        memcmp(s_stat[i].last, mac, OBSERVORE_MAC_LEN) != 0) {
        memcpy(s_stat[i].last, mac, OBSERVORE_MAC_LEN);
        s_stat[i].have_last = true;
        if (s_stat[i].addresses < UINT8_MAX) {
            s_stat[i].addresses++;
        }
    }
    if (!s_stat[i].disabled &&
        s_rules[i].kind == OBSERVORE_MUTE_FINGERPRINT &&
        s_stat[i].addresses > OBSERVORE_MUTE_ADDRESS_LIMIT) {
        s_stat[i].disabled = true;
        ESP_LOGW(TAG, "ignore rule %u (fingerprint %08lx) has covered %u "
                      "addresses -- it describes a kind of device, not one, "
                      "so it is no longer honoured",
                 (unsigned)i, (unsigned long)s_rules[i].fingerprint,
                 (unsigned)s_stat[i].addresses);
    }
}

bool observore_mute_stat(size_t index, observore_mute_stat_t *out)
{
    bool ok = false;
    MUTE_LOCK();
    if (index < s_count && out) {
        out->suppressed = s_stat[index].suppressed;
        out->addresses  = s_stat[index].addresses;
        out->disabled   = s_stat[index].disabled;
        ok = true;
    }
    MUTE_UNLOCK();
    return ok;
}

bool observore_mute_matches(const uint8_t mac[OBSERVORE_MAC_LEN], observore_class_t cls,
                        const char *name, uint32_t fingerprint)
{
    if (!mac) {
        return false;
    }
    bool hit = false;
    size_t hit_index = 0;

    MUTE_LOCK();
    for (size_t i = 0; i < s_count && !hit; i++) {
        const observore_mute_rule_t *r = &s_rules[i];
        switch (r->kind) {
            case OBSERVORE_MUTE_MAC:
                hit = memcmp(r->mac, mac, OBSERVORE_MAC_LEN) == 0;
                break;
            case OBSERVORE_MUTE_OUI:
                hit = memcmp(r->mac, mac, 3) == 0;
                break;
            case OBSERVORE_MUTE_CLASS:
                /* Never let a class rule swallow unclassified traffic, or
                 * muting "camera" would also disable the follower heuristic. */
                hit = cls != OBSERVORE_CLASS_UNKNOWN && r->cls == (uint8_t)cls;
                break;
            case OBSERVORE_MUTE_NAME:
                hit = name && observore_contains_ci(name, r->ssid);
                break;
            case OBSERVORE_MUTE_FINGERPRINT:
                /* Two safety rules, both enforced here rather than left to
                 * callers. A fingerprint identifies a kind of device, so it
                 * must never silence a threat -- and a fingerprint rule that
                 * has covered more addresses than one device could have stops
                 * being honoured at all. */
                hit = fingerprint != 0 && r->fingerprint == fingerprint &&
                      !observore_mute_class_is_protected(cls) &&
                      !s_stat[i].disabled;
                break;
            default:
                break;
        }
        if (hit) {
            hit_index = i;
        }
    }
    if (hit) {
        s_suppressed++;
        note_reach(hit_index, mac);
    }
    MUTE_UNLOCK();
    return hit;
}

/* ------------------------------------------------------------------ */
/* List management                                                    */
/* ------------------------------------------------------------------ */

static bool same_rule(const observore_mute_rule_t *a, const observore_mute_rule_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case OBSERVORE_MUTE_MAC:   return memcmp(a->mac, b->mac, OBSERVORE_MAC_LEN) == 0;
        case OBSERVORE_MUTE_OUI:   return memcmp(a->mac, b->mac, 3) == 0;
        case OBSERVORE_MUTE_CLASS: return a->cls == b->cls;
        case OBSERVORE_MUTE_NAME:  return strcmp(a->ssid, b->ssid) == 0;
        case OBSERVORE_MUTE_FINGERPRINT: return a->fingerprint == b->fingerprint;
        default:               return false;
    }
}

static esp_err_t mute_add(const observore_mute_rule_t *rule, bool *added,
                         bool persist)
{
    if (!rule || rule->kind >= OBSERVORE_MUTE_KIND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_NAME && rule->ssid[0] == '\0') {
        /* An empty substring matches everything. */
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_FINGERPRINT && rule->fingerprint == 0) {
        /* 0 means "no fingerprint", so such a rule would be meaningless. */
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_CLASS &&
        (rule->cls == OBSERVORE_CLASS_UNKNOWN || rule->cls >= OBSERVORE_CLASS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (added) {
        *added = false;
    }

    esp_err_t err = ESP_OK;
    MUTE_LOCK();
    for (size_t i = 0; i < s_count; i++) {
        if (same_rule(&s_rules[i], rule)) {
            MUTE_UNLOCK();
            return ESP_OK;  /* idempotent */
        }
    }
    if (s_count >= OBSERVORE_MUTE_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_rules[s_count++] = *rule;
        if (added) {
            *added = true;
        }
        if (persist) {
            mute_save();
        }
    }
    MUTE_UNLOCK();
    return err;
}

esp_err_t observore_mute_add(const observore_mute_rule_t *rule, bool *added)
{
    return mute_add(rule, added, true);
}

esp_err_t observore_mute_add_deferred(const observore_mute_rule_t *rule,
                                      bool *added)
{
    return mute_add(rule, added, false);
}

void observore_mute_save(void)
{
    MUTE_LOCK();
    mute_save();
    MUTE_UNLOCK();
}

esp_err_t observore_mute_remove(size_t index)
{
    esp_err_t err = ESP_OK;
    MUTE_LOCK();
    if (index >= s_count) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        memmove(&s_rules[index], &s_rules[index + 1],
                (s_count - index - 1) * sizeof(observore_mute_rule_t));
        s_count--;
        memset(&s_rules[s_count], 0, sizeof(observore_mute_rule_t));
        mute_save();
    }
    MUTE_UNLOCK();
    return err;
}

esp_err_t observore_mute_clear(void)
{
    MUTE_LOCK();
    s_count = 0;
    memset(s_rules, 0, sizeof(s_rules));
    mute_save();
    MUTE_UNLOCK();
    return ESP_OK;
}

size_t observore_mute_count(void)
{
    MUTE_LOCK();
    size_t n = s_count;
    MUTE_UNLOCK();
    return n;
}

bool observore_mute_get(size_t index, observore_mute_rule_t *out)
{
    if (!out) {
        return false;
    }
    MUTE_LOCK();
    bool ok = index < s_count;
    if (ok) {
        *out = s_rules[index];
    }
    MUTE_UNLOCK();
    return ok;
}

size_t observore_mute_list(observore_mute_rule_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    MUTE_LOCK();
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_rules, n * sizeof(observore_mute_rule_t));
    MUTE_UNLOCK();
    return n;
}

uint32_t observore_mute_suppressed(void)
{
    return s_suppressed;
}

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                    */
/* ------------------------------------------------------------------ */

const char *observore_mute_kind_name(observore_mute_kind_t kind)
{
    switch (kind) {
        case OBSERVORE_MUTE_MAC:   return "mac";
        case OBSERVORE_MUTE_OUI:   return "oui";
        case OBSERVORE_MUTE_CLASS: return "class";
        case OBSERVORE_MUTE_NAME:  return "name";
        case OBSERVORE_MUTE_FINGERPRINT: return "fingerprint";
        default:               return "?";
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool observore_mute_parse_mac(const char *text, uint8_t *out, size_t want)
{
    if (!text || !out || want == 0 || want > OBSERVORE_MAC_LEN) {
        return false;
    }
    size_t got = 0;
    const char *p = text;

    while (got < want) {
        while (*p == ':' || *p == '-' || *p == '.') {
            p++;
        }
        int hi = hex_nibble(*p);
        if (hi < 0) {
            return false;
        }
        int lo = hex_nibble(*(p + 1));
        if (lo < 0) {
            return false;
        }
        out[got++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }

    /* Reject trailing rubbish so a typo is an error rather than a rule that
     * silently matches the wrong device. */
    while (*p == ':' || *p == '-' || *p == '.') {
        p++;
    }
    return *p == '\0';
}

bool observore_mute_parse_class(const char *name, observore_class_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        if (strcmp(name, observore_class_name(c)) == 0) {
            *out = (observore_class_t)c;
            return true;
        }
    }
    return false;
}

/* Turn one tracked device into the most durable mute rule it supports and add
 * it, tallying the outcome into `r`. Pulled out of the baseline loop so that
 * loop can walk the table in chunks (see observore_mute_baseline). */
/* Fingerprints seen in this baseline, and how many devices carried each.
 *
 * A shape shared by two devices in one room is not a device, it is a model --
 * and a rule written against it silences every one of them, including the ones
 * that walk in tomorrow. Measured here rather than guessed: a baseline taken
 * in a house wrote three such rules and took a detector from three thousand
 * sightings an hour to twenty-five, reading "clear" throughout. */
#define FP_TALLY_MAX 48
static struct { uint32_t fp; uint8_t seen; } s_tally[FP_TALLY_MAX];
static size_t s_tally_n;

static void tally_add(uint32_t fp)
{
    if (fp == 0) {
        return;
    }
    for (size_t i = 0; i < s_tally_n; i++) {
        if (s_tally[i].fp == fp) {
            if (s_tally[i].seen < UINT8_MAX) {
                s_tally[i].seen++;
            }
            return;
        }
    }
    if (s_tally_n < FP_TALLY_MAX) {
        s_tally[s_tally_n].fp = fp;
        s_tally[s_tally_n].seen = 1;
        s_tally_n++;
    }
}

/* How many devices in this baseline share a shape. Anything the tally could
 * not hold is treated as shared, which errs towards the address rule -- the
 * conservative direction, since a MAC rule can only ever silence one device. */
static unsigned tally_count(uint32_t fp)
{
    for (size_t i = 0; i < s_tally_n; i++) {
        if (s_tally[i].fp == fp) {
            return s_tally[i].seen;
        }
    }
    return 2;
}

static void baseline_one(const observore_event_t *e, observore_baseline_t *r)
{
    observore_mute_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    /* Pick the most durable rule this device supports.
     *
     * A name is best: it survives address rotation and is specific enough
     * to mean one device ("Encharg/492232007683").
     *
     * Otherwise a fingerprint, which also survives rotation -- but it
     * matches a KIND of device, so it is not used for the classes where
     * that could hide a real threat.
     *
     * Otherwise the MAC, which for a rotating address buys only an hour
     * or so.  Counted as temporary and reported as such.
     *
     * A follower is the one protected class that gets a fingerprint here
     * anyway, when its address rotates. "Follower" is a verdict about
     * duration, not about what the device is: a phone that has sat in the
     * room for five minutes is promoted, and after its address rolls a MAC
     * rule is gone within the hour -- which is why the same handset was
     * being announced every day on the bench. A baseline is the owner
     * standing at the device saying "what is here now is mine", and the
     * household's phones are the whole point of that. The cost is real
     * and is stated: a stranger carrying the same model, advertising the
     * same way, is quiet too. A tracker, a drone, a camera or a body-worn
     * device keeps its protection, because those are classified by what
     * they are, and nothing about a baseline changes what they are. */
    bool rotating_follower = e->cls == OBSERVORE_CLASS_FOLLOWER &&
                             e->addr_random;
    /* Only where the shape belongs to exactly one device in front of us. */
    bool shape_is_one_device = e->fingerprint != 0 &&
                               tally_count(e->fingerprint) == 1;
    /* A name rule matches as a substring, which is right for a console where
     * somebody typed it deliberately and wrong for a baseline that takes
     * whatever a device happens to broadcast. A short name is a fragment: the
     * SSID "42" silenced a hundred different addresses on the bench, every one
     * of them something whose name merely contained those two characters. */
    bool name_is_specific = strlen(e->detail) >= OBSERVORE_BASELINE_NAME_MIN;
    if (e->detail[0] != '\0' && name_is_specific) {
        rule.kind = OBSERVORE_MUTE_NAME;
        snprintf(rule.ssid, sizeof(rule.ssid), "%s", e->detail);
    } else if (shape_is_one_device &&
               (!observore_mute_class_is_protected(e->cls) ||
                rotating_follower)) {
        rule.kind = OBSERVORE_MUTE_FINGERPRINT;
        rule.fingerprint = e->fingerprint;
    } else {
        rule.kind = OBSERVORE_MUTE_MAC;
        memcpy(rule.mac, e->mac, OBSERVORE_MAC_LEN);
    }

    bool is_new = false;
    esp_err_t err = observore_mute_add_deferred(&rule, &is_new);
    if (err == ESP_ERR_NO_MEM) {
        r->no_room++;
        return;
    }
    if (err != ESP_OK) {
        return;
    }
    if (!is_new) {
        r->already++;
        return;
    }
    r->added++;
    switch (rule.kind) {
        case OBSERVORE_MUTE_NAME:        r->by_name++; break;
        case OBSERVORE_MUTE_FINGERPRINT: r->by_fingerprint++; break;
        default:
            r->by_mac++;
            if (e->addr_random) {
                r->temporary++;
            }
            break;
    }
}

void observore_mute_baseline(observore_event_t *scratch, size_t cap,
                             observore_baseline_t *out)
{
    observore_baseline_t r = {0};

    /* Walk the whole table in chunks of `cap`, so this needs only a few-KB
     * scratch rather than one big enough for every slot at once. A snapshot of
     * all 192 slots is ~23 KB, which a board with no PSRAM has no single free
     * block for once the heap is a little fragmented -- that malloc failing is
     * exactly what put "baseline failed: out of memory" on the 3.5" CYD's
     * screen. Chunking also fixes the web path, which previously baselined only
     * the first `cap` (24) devices on such a board and silently left the rest. */
    /* Two passes over the table: the first counts how many devices carry each
     * advert shape, the second writes the rules. A single pass cannot know
     * whether the shape in front of it is shared, and that is the whole
     * question. */
    size_t cursor = 0, got;
    s_tally_n = 0;
    do {
        got = observore_track_all_from(scratch, cap, &cursor);
        for (size_t i = 0; i < got; i++) {
            tally_add(scratch[i].fingerprint);
        }
    } while (got == cap);

    cursor = 0;
    do {
        got = observore_track_all_from(scratch, cap, &cursor);
        r.seen += got;
        for (size_t i = 0; i < got; i++) {
            baseline_one(&scratch[i], &r);
        }
    } while (got == cap);

    /* One flash write for the whole baseline rather than one per rule. */
    observore_mute_save();

    /* Everything in range is now known, so the score and the log start from
     * a clean slate -- that is what makes it a baseline rather than just a
     * bulk mute. */
    observore_track_clear();

    if (out) {
        *out = r;
    }
}
