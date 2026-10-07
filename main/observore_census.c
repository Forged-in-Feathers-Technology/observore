/* localtime_r is POSIX, and -std=c11 without this hides it. On the device
 * ESP-IDF's headers expose it regardless; this is for the host build, which
 * compiles strictly and treats the implicit declaration as an error. */
#ifdef OBSERVORE_HOST_TEST
#define _POSIX_C_SOURCE 200809L
#endif

#include "observore_census.h"
#include "observore_mute.h"

#include <string.h>

#ifdef OBSERVORE_HOST_TEST
/* No flash and no log on the host: the day arithmetic and the membership rule
 * are the whole of what is worth testing, and both are pure. */
static void census_save(void) {}
static void census_load(void) {}
#else
#include "esp_log.h"
#include "observore_nvs.h"
static const char *TAG = "observore.census";
static void census_save(void);
static void census_load(void);
#endif

/* The stored blob and the live table are the same bytes.
 *
 * They were separate, and that cost 4,864 bytes of static RAM on every board:
 * the table, plus a scratch buffer in the loader and another in the saver, all
 * the same size. On the devkit C5 -- headless, so it carries the notifier and
 * its TLS -- the internal heap floor fell from about fourteen kilobytes to
 * 2,932 over one night, and name resolution began failing with EAI_FAIL while
 * notifications queued up unsent. getaddrinfo() has to allocate, and at three
 * kilobytes it could not.
 *
 * Laying the header immediately before the entries means a save hands NVS a
 * pointer into this struct and a length, with nothing copied anywhere. The
 * loader reads straight into it and validates afterwards, which is safe
 * because a blob that fails validation leaves the census empty -- which is
 * exactly what a rejected blob means anyway.
 *
 * The header is four bytes, so the entries stay four-aligned behind it and
 * their uint32 ids need no packing. */
typedef struct {
    uint16_t version;
    uint16_t count;
} census_blob_hdr_t;

static struct {
    census_blob_hdr_t        hdr;
    observore_census_entry_t e[OBSERVORE_CENSUS_MAX];
} s_store = {.hdr = {OBSERVORE_CENSUS_FORMAT, 0}};

#define s_tab   (s_store.e)
#define s_count (s_store.hdr.count)

/* Days from 2000-01-01, from a civil date.
 *
 * Howard Hinnant's days_from_civil, with the era shifted to 2000 so the
 * result fits a uint16_t until 2179 and a device's whole life is a small
 * number. Written out rather than taken from mktime() because mktime()
 * normalises against the current zone and can fail, and this needs to be the
 * same arithmetic on the host as on the device. */
static int days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 730425;  /* 730425 = days to 2000-01-01 */
}

int observore_census_day_from_tm(const struct tm *lt)
{
    if (!lt) {
        return OBSERVORE_CENSUS_NO_DAY;
    }
    /* A year before 2000 is a clock that has not been set rather than a
     * device that has travelled: ESP-IDF starts at 1970, and every sighting
     * before the first SNTP reply lands there. */
    int year = lt->tm_year + 1900;
    if (year < 2000) {
        return OBSERVORE_CENSUS_NO_DAY;
    }
    int day = days_from_civil(year, (unsigned)lt->tm_mon + 1u,
                              (unsigned)lt->tm_mday);
    if (day < 0 || day > OBSERVORE_CENSUS_DAY_MAX) {
        return OBSERVORE_CENSUS_NO_DAY;
    }
    return day;
}

int observore_census_day(time_t when)
{
    struct tm lt;
    if (!localtime_r(&when, &lt)) {
        return OBSERVORE_CENSUS_NO_DAY;
    }
    return observore_census_day_from_tm(&lt);
}

uint32_t observore_census_id(const observore_event_t *e)
{
    if (!e) {
        return 0;
    }
    if (e->fingerprint != 0) {
        return e->fingerprint;
    }
    if (e->addr_random) {
        return 0;
    }
    /* FNV-1a over the address, offset basis nudged so a MAC hash and a
     * fingerprint cannot collide by being the same arithmetic over different
     * inputs. */
    uint32_t h = 0x811C9DC5u ^ 0x4D41435Bu;   /* "MAC[" */
    for (int i = 0; i < OBSERVORE_MAC_LEN; i++) {
        h = (h ^ e->mac[i]) * 16777619u;
    }
    return h ? h : 1u;   /* 0 means "no identity", so never return it */
}

observore_census_verdict_t observore_census_verdict(uint32_t id, int day,
                                                    observore_class_t cls)
{
    /* Checked first, and before membership, so that no later change to this
     * function can reach a protected class by some other path. The classes
     * this device exists to find are not the census's to touch -- not to
     * silence and not to quieten, because reducing the alarm on a body camera
     * is still reducing it. */
    if (observore_mute_class_is_protected(cls)) {
        return OBSERVORE_CENSUS_REPORT;
    }
    if (id == 0 || !observore_census_is_household(id, day)) {
        return OBSERVORE_CENSUS_REPORT;
    }
    bool over = false;
    int addrs = observore_census_addresses(id, &over);
    if (over || addrs >= OBSERVORE_CENSUS_ADDRS) {
        return OBSERVORE_CENSUS_DAMPEN;
    }
    return OBSERVORE_CENSUS_QUIET;
}

uint16_t observore_census_dampen(uint16_t points)
{
    uint16_t half = (uint16_t)(points / 2);
    return half > 0 ? half : (points > 0 ? 1u : 0u);
}

void observore_census_init(void)
{
    s_count = 0;
    memset(s_tab, 0, sizeof(s_tab));
    census_load();
}

static observore_census_entry_t *find(uint32_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_tab[i].id == id) {
            return &s_tab[i];
        }
    }
    return NULL;
}

/* Move an entry's window forward to `day`.
 *
 * The shift distance is bounded before it is used, which is the whole reason
 * this is a function. A shift of sixteen or more on a uint16_t is undefined
 * behaviour, not a zero, and the device is *guaranteed* to ask for one: every
 * run starts with the clock unset, and when SNTP answers, the day number
 * jumps from nothing to about nine thousand. Anything stored before that is
 * further in the past than the window is wide, so the answer is an empty
 * mask -- the same thing the shift would mean if C defined it. */
static void age_to(observore_census_entry_t *e, int day)
{
    int moved = day - (int)e->last_day;
    if (moved <= 0) {
        /* Not newer. A clock corrected backwards -- NTP stepping a device
         * that had drifted, or a timezone change -- must not rewrite history
         * it cannot reconstruct, so the window stands and today's sighting
         * lands on the day already at bit 0. Losing a day's resolution is a
         * better failure than shifting the mask the wrong way. */
        return;
    }
    if (moved >= OBSERVORE_CENSUS_WINDOW) {
        e->days = 0;
    } else {
        e->days = (uint16_t)(e->days << moved);
    }
    e->last_day = (uint16_t)day;
}

static int popcount16(uint16_t v)
{
    int n = 0;
    while (v) {
        v &= (uint16_t)(v - 1);
        n++;
    }
    return n;
}

/* Sixteen bits of FNV-1a over the six address bytes.
 *
 * Collisions are possible and are a known, bounded cost: with eight slots in
 * a 65536-wide space, two of a device's addresses colliding is rare, and the
 * effect when it happens is to under-count by one. Under-counting makes the
 * measurement optimistic, which is worth saying out loud because it is being
 * used to judge whether suppression is safe. */
static uint16_t addr_hash(const uint8_t *mac)
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

/* Record an address against an entry, if it is one we have not seen. */
static bool note_addr(observore_census_entry_t *e, const uint8_t *mac)
{
    if (!mac) {
        return false;
    }
    uint16_t h = addr_hash(mac);
    for (uint8_t i = 0; i < e->addr_n; i++) {
        if (e->addr[i] == h) {
            return false;        /* already counted */
        }
    }
    if (e->addr_n < OBSERVORE_CENSUS_ADDRS) {
        e->addr[e->addr_n++] = h;
        return true;
    }
    /* The set is full and this is a further address. The count saturates, but
     * that it saturated is itself worth recording: at the limit, "eight" and
     * "eight and still arriving" mean different things about the device. */
    if (!e->addr_over) {
        e->addr_over = 1;
        return true;
    }
    return false;
}

void observore_census_note(uint32_t id, int day, const uint8_t *mac)
{
    if (day < 0 || day > OBSERVORE_CENSUS_DAY_MAX) {
        return;
    }

    observore_census_entry_t *e = find(id);
    if (!e) {
        if (s_count < OBSERVORE_CENSUS_MAX) {
            e = &s_tab[s_count++];
        } else {
            /* Full. The entry with the oldest day goes, which on a table this
             * size means the thing least likely to be furniture: anything
             * genuinely around every day is, by construction, among the most
             * recently seen. */
            e = &s_tab[0];
            for (size_t i = 1; i < s_count; i++) {
                if (s_tab[i].last_day < e->last_day) {
                    e = &s_tab[i];
                }
            }
        }
        e->id        = id;
        e->days      = 0;
        e->last_day  = (uint16_t)day;
        e->addr_n    = 0;
        e->addr_over = 0;
        memset(e->addr, 0, sizeof(e->addr));
    }

    age_to(e, day);
    uint16_t before = e->days;
    e->days |= 1u;
    bool new_addr = note_addr(e, mac);
    /* Only a new day or a new address is worth a write. A doorbell seen four
     * hundred times an hour would otherwise wear the flash out to record a
     * fact that has not changed since breakfast. */
    if (e->days != before || new_addr) {
        census_save();
    }
}

int observore_census_addresses(uint32_t id, bool *over)
{
    observore_census_entry_t *e = find(id);
    if (over) {
        *over = e ? (e->addr_over != 0) : false;
    }
    return e ? (int)e->addr_n : 0;
}

static uint16_t window_of(uint32_t id, int day)
{
    observore_census_entry_t *e = find(id);
    if (!e || day < 0) {
        return 0;
    }
    /* A day past what the record can hold is further away than the window is
     * wide, whatever it is, so the honest answer is an empty window rather
     * than arithmetic on a number that could not have been stored. */
    if (day > OBSERVORE_CENSUS_DAY_MAX) {
        return 0;
    }
    /* Asked about a day in the past, the stored window is the best answer
     * there is; asked about a day in the future, age a copy rather than the
     * entry, because a question must not change what is known. */
    observore_census_entry_t copy = *e;
    age_to(&copy, day);
    return copy.days;
}

int observore_census_days_seen(uint32_t id, int day)
{
    return popcount16(window_of(id, day));
}

bool observore_census_is_household(uint32_t id, int day)
{
    return observore_census_days_seen(id, day) >= OBSERVORE_CENSUS_MIN_DAYS;
}

void observore_census_counts(int day, int *household, int *tracked)
{
    if (tracked) {
        *tracked = (int)s_count;
    }
    if (household) {
        int n = 0;
        for (size_t i = 0; i < s_count; i++) {
            if (observore_census_is_household(s_tab[i].id, day)) {
                n++;
            }
        }
        *household = n;
    }
}

void observore_census_addr_spread(int *one, int *few, int *many)
{
    int a = 0, b = 0, c = 0;
    for (size_t i = 0; i < s_count; i++) {
        /* Saturated counts as "many" whatever the stored number says: at the
         * ceiling the count has stopped being a count, and that is exactly
         * the case this is being read to detect. */
        if (s_tab[i].addr_over || s_tab[i].addr_n >= OBSERVORE_CENSUS_ADDRS) {
            c++;
        } else if (s_tab[i].addr_n >= 2) {
            b++;
        } else if (s_tab[i].addr_n == 1) {
            a++;
        }
        /* An identity with no address at all -- a Wi-Fi sighting with nothing
         * to attribute -- is in none of the buckets. It says nothing either
         * way about whether a fingerprint names one device. */
    }
    if (one)  { *one = a; }
    if (few)  { *few = b; }
    if (many) { *many = c; }
}

int observore_census_household_at_ceiling(int day)
{
    int n = 0;
    for (size_t i = 0; i < s_count; i++) {
        bool at_cap = s_tab[i].addr_over ||
                      s_tab[i].addr_n >= OBSERVORE_CENSUS_ADDRS;
        if (at_cap && observore_census_is_household(s_tab[i].id, day)) {
            n++;
        }
    }
    return n;
}

size_t observore_census_entries(const observore_census_entry_t **out)
{
    if (out) {
        *out = s_tab;
    }
    return s_count;
}

size_t observore_census_blob(void *out, size_t cap)
{
    size_t need = sizeof(census_blob_hdr_t) +
                  s_count * sizeof(observore_census_entry_t);
    if (!out || cap < need) {
        return need;              /* how much room it wants */
    }
    census_blob_hdr_t hdr = {OBSERVORE_CENSUS_FORMAT, (uint16_t)s_count};
    memcpy(out, &hdr, sizeof(hdr));
    memcpy((char *)out + sizeof(hdr), s_tab,
           s_count * sizeof(observore_census_entry_t));
    return need;
}

bool observore_census_restore(const void *blob, size_t len)
{
    census_blob_hdr_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    if (hdr.version != OBSERVORE_CENSUS_FORMAT) {
        return false;             /* written by other code; not guessed at */
    }
    if (hdr.count > OBSERVORE_CENSUS_MAX) {
        return false;
    }
    /* The length must be exactly what the header claims. A blob that is
     * longer, or short of its last entry, is not a table this code wrote. */
    if (len != sizeof(hdr) + (size_t)hdr.count * sizeof(observore_census_entry_t)) {
        return false;
    }
    /* The loader reads straight into the store, so the source and the
     * destination are the same bytes and there is nothing to move. memcpy
     * with identical pointers is undefined behaviour rather than a harmless
     * no-op, so it is skipped rather than relied upon. */
    const void *src = (const char *)blob + sizeof(hdr);
    if (src != (const void *)s_tab) {
        memcpy(s_tab, src, (size_t)hdr.count * sizeof(observore_census_entry_t));
    }
    s_count = hdr.count;
    return true;
}

#ifndef OBSERVORE_HOST_TEST
static void census_load(void)
{
    /* Straight into the store. A blob that does not validate leaves the
     * census empty, which is what rejecting it means, so there is nothing to
     * protect by reading somewhere else first. */
    observore_nvs_item_t item = {.key = "census", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &s_store, .len = sizeof(s_store)};
    if (observore_nvs_read(&item, 1) != ESP_OK || !item.found) {
        s_count = 0;
        return;
    }
    if (!observore_census_restore(&s_store, item.len)) {
        ESP_LOGW(TAG, "the saved census was written by an older build "
                      "(%u bytes) -- starting over. It will rebuild over the "
                      "next few days.", (unsigned)item.len);
        s_store.hdr.version = OBSERVORE_CENSUS_FORMAT;
        s_count = 0;
        return;
    }
    /* The spread as well as the count, at a level that survives a release
     * build.
     *
     * The system page shows this, which covers four of the boards here and not
     * the fifth: the devkit is headless, and /api/census needs the console
     * password. So the one board that can only ever be read over a cable was
     * the one board that could not report the measurement the census exists to
     * produce. A log line costs nothing and covers every board.
     *
     * The day is not known yet at load time -- the clock arrives with the
     * first uplink -- so membership cannot be judged here and the at-cap
     * figure is left to the screen and the console. The spread does not depend
     * on the day, because addresses do not decay. */
    int one = 0, few = 0, many = 0;
    observore_census_addr_spread(&one, &few, &many);
    ESP_LOGI(TAG, "census: %u known -- addresses %dx1 %dx2-%d %dx%d+",
             (unsigned)s_count, one, few, OBSERVORE_CENSUS_ADDRS - 1,
             many, OBSERVORE_CENSUS_ADDRS);
    if (many > 0) {
        /* Said plainly because it is the finding that decides what the census
         * may do, not a statistic: at the ceiling an advert shape names a
         * population rather than a device. */
        ESP_LOGW(TAG, "%d identit%s wear %d or more addresses -- a fingerprint "
                      "there names a kind of device, not one",
                 many, many == 1 ? "y" : "ies", OBSERVORE_CENSUS_ADDRS);
    }
}

static void census_save(void)
{
    /* No copy: the header already sits in front of the entries. */
    s_store.hdr.version = OBSERVORE_CENSUS_FORMAT;
    const observore_nvs_item_t item = {
        .key = "census", .type = OBSERVORE_NVS_BLOB, .buf = &s_store,
        .len = sizeof(census_blob_hdr_t) +
               (size_t)s_count * sizeof(observore_census_entry_t)};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the census");
    }
}
#endif
