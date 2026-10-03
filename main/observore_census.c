/* localtime_r is POSIX, and -std=c11 without this hides it. On the device
 * ESP-IDF's headers expose it regardless; this is for the host build, which
 * compiles strictly and treats the implicit declaration as an error. */
#ifdef OBSERVORE_HOST_TEST
#define _POSIX_C_SOURCE 200809L
#endif

#include "observore_census.h"

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

static observore_census_entry_t s_tab[OBSERVORE_CENSUS_MAX];
static size_t s_count;

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

size_t observore_census_entries(const observore_census_entry_t **out)
{
    if (out) {
        *out = s_tab;
    }
    return s_count;
}

/* What a saved census looks like: a short header, then the entries.
 *
 * The header exists because the entries grew. Version 1 had no address set
 * and was eight bytes an entry; this one is larger, and an old blob holding
 * an even number of entries divides evenly into the new entry size. It would
 * have restored as half as many entries of garbage -- plausible-looking ids,
 * nonsense day masks -- rather than being rejected. Recognising a format by
 * the length that happens to fit is how the touch calibration went wrong, so
 * this says what wrote it. */
typedef struct {
    uint16_t version;
    uint16_t count;
} census_blob_hdr_t;

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
    memcpy(s_tab, (const char *)blob + sizeof(hdr),
           (size_t)hdr.count * sizeof(observore_census_entry_t));
    s_count = hdr.count;
    return true;
}

#ifndef OBSERVORE_HOST_TEST
#define CENSUS_BLOB_MAX (sizeof(census_blob_hdr_t) + \
                         OBSERVORE_CENSUS_MAX * sizeof(observore_census_entry_t))

static void census_load(void)
{
    static uint8_t buf[CENSUS_BLOB_MAX];
    observore_nvs_item_t item = {.key = "census", .type = OBSERVORE_NVS_BLOB,
                                 .buf = buf, .len = sizeof(buf)};
    if (observore_nvs_read(&item, 1) != ESP_OK || !item.found) {
        return;
    }
    if (!observore_census_restore(buf, item.len)) {
        ESP_LOGW(TAG, "the saved census was written by an older build "
                      "(%u bytes) -- starting over. It will rebuild over the "
                      "next few days.", (unsigned)item.len);
        return;
    }
    ESP_LOGI(TAG, "census: %u known", (unsigned)s_count);
}

static void census_save(void)
{
    static uint8_t buf[CENSUS_BLOB_MAX];
    size_t n = observore_census_blob(buf, sizeof(buf));
    const observore_nvs_item_t item = {
        .key = "census", .type = OBSERVORE_NVS_BLOB, .buf = buf, .len = n};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the census");
    }
}
#endif
