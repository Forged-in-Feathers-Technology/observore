/* localtime_r is POSIX, and -std=c11 without this hides it. On the device
 * ESP-IDF's headers expose it regardless; this is for the host build, which
 * compiles strictly and treats the implicit declaration as an error. */
#ifdef OBSERVORE_HOST_TEST
#define _POSIX_C_SOURCE 200809L
#endif

#include "observore_census.h"
#include "observore_mute.h"

#include <stdio.h>
#include <string.h>

/* One lock over both tables.
 *
 * The census is written by the main task and read by the HTTP task, the
 * display task and, with the exchange, the radio path's merges, and on the
 * dual-core boards they really do run at once. Every public function that
 * reads or writes either table takes it once and gives it before it returns,
 * and nothing hands out a pointer into a table, so a reader can only ever see
 * a copy. It is recursive because the tracker's lock is held when the verdict
 * is asked for, and static so it needs no heap and no creation order beyond
 * observore_census_init(), which app_main runs before the console, the screen,
 * the BLE host and the radio path exist.
 *
 * On the host the suite is single-threaded and the macros are empty, as the
 * tracker's are. */
#ifdef OBSERVORE_HOST_TEST
#define CENSUS_LOCK()   do {} while (0)
#define CENSUS_UNLOCK() do {} while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
#define CENSUS_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define CENSUS_UNLOCK() xSemaphoreGiveRecursive(s_lock)
#endif

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

#if CONFIG_OBSERVORE_MESH_CENSUS
/* The same layout for what the owner's other nodes reported: the stored blob
 * and the live table are the same bytes, so a save is a pointer and a length.
 *
 * The header carries four bytes of a check value derived from the household
 * secret, so a table written under one secret is not read back under another.
 * Eight bytes in all, which keeps the 36-byte entries four-aligned behind it. */
typedef struct {
    uint16_t version;
    uint16_t count;
    uint8_t  check[OBSERVORE_PEER_CENSUS_CHECK_LEN];
} census_peer_hdr_t;

_Static_assert(sizeof(census_peer_hdr_t) == 8,
               "the peer blob header is eight bytes, so its entries stay four-aligned");

static struct {
    census_peer_hdr_t             hdr;
    observore_peer_census_entry_t e[OBSERVORE_PEER_CENSUS_MAX];
} s_peer = {.hdr = {.version = OBSERVORE_PEER_CENSUS_FORMAT}};

#define p_tab   (s_peer.e)
#define p_count (s_peer.hdr.count)

/* Why the peer table last emptied, so the console can say so rather than show
 * an empty table with no explanation, and whether it differs from what was
 * last saved. */
static observore_census_forget_t s_peer_forgot = OBSERVORE_CENSUS_FORGOT_NONE;
static bool                      s_peer_changed;
#endif

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

uint16_t observore_census_dampen(uint16_t points)
{
    uint16_t half = (uint16_t)(points / 2);
    return half > 0 ? half : (points > 0 ? 1u : 0u);
}

void observore_census_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    }
#endif
    s_count = 0;
    memset(s_tab, 0, sizeof(s_tab));
#if CONFIG_OBSERVORE_MESH_CENSUS
    p_count = 0;
    memset(p_tab, 0, sizeof(p_tab));
    memset(s_peer.hdr.check, 0, sizeof(s_peer.hdr.check));
    s_peer_forgot  = OBSERVORE_CENSUS_FORGOT_NONE;
    s_peer_changed = false;
#endif
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

/* A day mask as it reads from another day.
 *
 * Bit 0 of `mask` is `from_day`; the result has bit 0 as `to_day`. Later days
 * shift left, and the shift distance is bounded before it is used: a shift of
 * sixteen or more on a uint16_t is undefined behaviour, not a zero, and the
 * device is *guaranteed* to ask for one -- every run starts with the clock
 * unset, and when SNTP answers, the day number jumps from nothing to about
 * nine thousand. Anything stored before that is further in the past than the
 * window is wide, so the answer is an empty mask.
 *
 * Earlier days shift right, which is what a friend whose clock is a day ahead
 * needs. It reports its own today, which is this node's tomorrow, and counting
 * that day now would count a day early. The local window never asks for this:
 * age_to() below keeps its rule that a past day leaves the stored window as it
 * is. */
static uint16_t align_mask(uint16_t mask, int from_day, int to_day)
{
    int d = to_day - from_day;
    if (d == 0) {
        return mask;
    }
    if (d > 0) {
        return d >= OBSERVORE_CENSUS_WINDOW ? 0 : (uint16_t)(mask << d);
    }
    d = -d;
    return d >= OBSERVORE_CENSUS_WINDOW ? 0 : (uint16_t)(mask >> d);
}

/* Move an entry's window forward to `day`, by align_mask()'s bounded shift. */
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
    e->days = align_mask(e->days, e->last_day, day);
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

/* Record an address hash in a set, if it is one the set has not seen.
 *
 * One rule for this node's own addresses and for the union a friend's
 * addresses are merged into, so the ceiling means the same thing on both
 * sides. Returns whether anything changed. */
static bool note_hash(uint16_t *addr, uint8_t *addr_n, uint8_t *addr_over,
                      uint16_t h)
{
    for (uint8_t i = 0; i < *addr_n; i++) {
        if (addr[i] == h) {
            return false;        /* already counted */
        }
    }
    if (*addr_n < OBSERVORE_CENSUS_ADDRS) {
        addr[(*addr_n)++] = h;
        return true;
    }
    /* The set is full and this is a further address. The count saturates, but
     * that it saturated is itself worth recording: at the limit, "eight" and
     * "eight and still arriving" mean different things about the device. */
    if (!*addr_over) {
        *addr_over = 1;
        return true;
    }
    return false;
}

/* Record an address against an entry, if it is one we have not seen. */
static bool note_addr(observore_census_entry_t *e, const uint8_t *mac)
{
    if (!mac) {
        return false;
    }
    return note_hash(e->addr, &e->addr_n, &e->addr_over, addr_hash(mac));
}

/* Which entry gives up its place when the table is full.
 *
 * The first version of this took the entry with the oldest `last_day`, on the
 * reasoning that anything genuinely around every day is among the most
 * recently seen. The reasoning is sound; the implementation of it had a hole.
 *
 * On a stationary board everything in range is seen again every day, so every
 * entry carries today's date, no entry is older than any other, and a scan
 * for the smallest `last_day` returns the first slot every time -- whatever
 * is in it. One slot becomes a revolving door while the rest never move, and
 * what is in that slot may be two days into becoming furniture while
 * fifty-four single-day strangers sit untouched beside it. The cost is
 * bounded -- one entry, not the table -- and it is paid by the same entry
 * every time, so that identity can never establish itself for as long as the
 * device runs.
 *
 * The date was never the thing worth ranking on. How much an entry has shown
 * is.
 *
 * So the ranking is explicit, worst candidate first:
 *
 *   1. Not household before household. The table exists to remember
 *      furniture; evicting furniture to make room for a stranger is the one
 *      move that defeats the whole structure.
 *   2. Fewer distinct days before more. A single-day entry has shown nothing
 *      yet; a two-day entry is one evening away from being furniture.
 *   3. Older before newer, which is the original rule kept as the tie-break
 *      it should always have been.
 *
 * `day` is passed in because household-ness is a claim about now: an entry
 * with three days set a fortnight ago is not furniture any more, and its
 * place is exactly the one that should be given up. */
bool observore_census_full(void)
{
    CENSUS_LOCK();
    bool full = s_count >= OBSERVORE_CENSUS_MAX;
    CENSUS_UNLOCK();
    return full;
}

static observore_census_entry_t *evict(int day)
{
    observore_census_entry_t *worst = &s_tab[0];
    int worst_hh = 0, worst_days = 0;
    bool first = true;

    for (size_t i = 0; i < s_count; i++) {
        observore_census_entry_t *c = &s_tab[i];
        /* Against `day`, not against the stored mask: the window has to be
         * rolled forward before the bits mean anything, and doing it here
         * would mutate an entry we are only looking at. */
        int moved = day - (int)c->last_day;
        uint16_t mask = (moved <= 0) ? c->days
                                     : align_mask(c->days, c->last_day, day);
        int days = popcount16(mask);
        int hh = (days >= OBSERVORE_CENSUS_MIN_DAYS) ? 1 : 0;

        if (first ||
            hh < worst_hh ||
            (hh == worst_hh && days < worst_days) ||
            (hh == worst_hh && days == worst_days &&
             c->last_day < worst->last_day)) {
            worst = c;
            worst_hh = hh;
            worst_days = days;
            first = false;
        }
    }
    return worst;
}

void observore_census_note(uint32_t id, int day, const uint8_t *mac)
{
    if (day < 0 || day > OBSERVORE_CENSUS_DAY_MAX) {
        return;
    }

    CENSUS_LOCK();
    observore_census_entry_t *e = find(id);
    if (!e) {
        if (s_count < OBSERVORE_CENSUS_MAX) {
            e = &s_tab[s_count++];
        } else {
            e = evict(day);
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
    CENSUS_UNLOCK();
}

int observore_census_addresses(uint32_t id, bool *over)
{
    CENSUS_LOCK();
    observore_census_entry_t *e = find(id);
    int n = e ? (int)e->addr_n : 0;
    if (over) {
        *over = e ? (e->addr_over != 0) : false;
    }
    CENSUS_UNLOCK();
    return n;
}

static uint16_t window_at(const observore_census_entry_t *e, int day)
{
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

static uint16_t window_of(uint32_t id, int day)
{
    return window_at(find(id), day);
}

int observore_census_days_seen(uint32_t id, int day)
{
    CENSUS_LOCK();
    int n = popcount16(window_of(id, day));
    CENSUS_UNLOCK();
    return n;
}

bool observore_census_is_household(uint32_t id, int day)
{
    CENSUS_LOCK();
    bool hh = popcount16(window_of(id, day)) >= OBSERVORE_CENSUS_MIN_DAYS;
    CENSUS_UNLOCK();
    return hh;
}

#if CONFIG_OBSERVORE_MESH_CENSUS
static observore_peer_census_entry_t *find_peer(uint32_t id)
{
    for (size_t i = 0; i < p_count; i++) {
        if (p_tab[i].id == id) {
            return &p_tab[i];
        }
    }
    return NULL;
}
#endif

/* Everything the verdict, the explanation and a console row need to know about
 * one identity as of one day, worked out in one place so that they cannot
 * disagree. `local_*` is this node's own days and addresses alone; the rest is
 * the same joined with what the owner's other nodes reported. Without the
 * exchange the two are the same by construction. */
typedef struct {
    uint16_t local_mask;
    uint16_t mask;
    int      local_addrs;
    bool     local_over;
    int      addrs;
    bool     over;
    bool     here;                    /* in this node's own table */
    uint16_t last_day;
    uint8_t  from_n;
    uint16_t from[OBSERVORE_CENSUS_FROM];
} census_view_t;

static void view_of(uint32_t id, int day, census_view_t *v)
{
    memset(v, 0, sizeof(*v));
    const observore_census_entry_t *e = find(id);
    v->here        = e != NULL;
    v->local_mask  = window_at(e, day);
    v->local_addrs = e ? (int)e->addr_n : 0;
    v->local_over  = e ? (e->addr_over != 0) : false;
    v->last_day    = e ? e->last_day : 0;
    v->mask        = v->local_mask;
    v->addrs       = v->local_addrs;
    v->over        = v->local_over;
#if CONFIG_OBSERVORE_MESH_CENSUS
    const observore_peer_census_entry_t *p =
        (day >= 0 && day <= OBSERVORE_CENSUS_DAY_MAX) ? find_peer(id) : NULL;
    if (!p) {
        return;
    }
    if (!e) {
        v->last_day = p->last_day;
    }
    /* Every mask is read in the day the local window is read in: `day`, or the
     * local entry's own last day when that is later, which is what
     * window_at() returns the local mask against. With them on one day the
     * union is a plain OR.
     *
     * Two properties follow from the construction and are what make this safe
     * to act on. The merged mask contains the local mask, so a merge can never
     * make a device this node already quiets or dampens report again; and the
     * merged address set contains the local set, so a merge can only move an
     * identity from QUIET toward DAMPEN, never the other way. A friend's day
     * that is still in this node's future has been shifted out of the mask
     * by align_mask(), so it does not count until this node's day reaches it. */
    int a = (e && e->last_day > day) ? (int)e->last_day : day;
    uint16_t mask = v->local_mask;
    uint16_t nodes[OBSERVORE_CENSUS_FROM];
    uint8_t n = 0;
    for (int k = 0; k < OBSERVORE_CENSUS_FROM; k++) {
        if (p->from[k].node == 0) {
            continue;
        }
        uint16_t m = align_mask(p->from[k].days, p->last_day, a);
        if (m) {
            mask |= m;
            nodes[n++] = p->from[k].node;
        }
    }
    if (n == 0) {
        return;           /* every friend's days have aged out: nothing to add */
    }
    for (uint8_t i = 1; i < n; i++) {         /* ascending, three at most */
        uint16_t key = nodes[i];
        uint8_t j = i;
        while (j > 0 && nodes[j - 1] > key) {
            nodes[j] = nodes[j - 1];
            j--;
        }
        nodes[j] = key;
    }
    uint16_t addr[OBSERVORE_CENSUS_ADDRS] = {0};
    uint8_t an = 0, over = 0;
    if (e) {
        memcpy(addr, e->addr, sizeof(addr));
        an   = e->addr_n;
        over = e->addr_over;
    }
    uint8_t pn = p->addr_n > OBSERVORE_CENSUS_ADDRS ? OBSERVORE_CENSUS_ADDRS
                                                    : p->addr_n;
    for (uint8_t i = 0; i < pn; i++) {
        note_hash(addr, &an, &over, p->addr[i]);
    }
    if (p->addr_over) {
        over = 1;
    }
    v->mask   = mask;
    v->addrs  = (int)an;
    v->over   = over != 0;
    v->from_n = n;
    memcpy(v->from, nodes, n * sizeof(nodes[0]));
#endif
}

/* The ladder, from a day mask and an address set: not household reports;
 * household at the address ceiling dampens; household below it quiets. */
static observore_census_verdict_t ladder(uint16_t mask, int addrs, bool over)
{
    if (popcount16(mask) < OBSERVORE_CENSUS_MIN_DAYS) {
        return OBSERVORE_CENSUS_REPORT;
    }
    if (over || addrs >= OBSERVORE_CENSUS_ADDRS) {
        return OBSERVORE_CENSUS_DAMPEN;
    }
    return OBSERVORE_CENSUS_QUIET;
}

observore_census_verdict_t observore_census_explain(uint32_t id, int day,
                                                    observore_class_t cls,
                                                    observore_census_why_t *why)
{
    /* Checked first, and before membership or any table, so that no later
     * change to this function can reach a protected class by some other path.
     * The classes this device exists to find are not the census's to touch --
     * not to silence and not to quieten, because reducing the alarm on a body
     * camera is still reducing it. That holds for days this node counted and
     * for days another node reported alike. */
    if (observore_mute_class_is_protected(cls)) {
        if (why) {
            memset(why, 0, sizeof(*why));
            why->local  = OBSERVORE_CENSUS_REPORT;
            why->merged = OBSERVORE_CENSUS_REPORT;
        }
        return OBSERVORE_CENSUS_REPORT;
    }
    if (id == 0) {
        if (why) {
            memset(why, 0, sizeof(*why));
        }
        return OBSERVORE_CENSUS_REPORT;
    }
    census_view_t v;
    CENSUS_LOCK();
    view_of(id, day, &v);
    CENSUS_UNLOCK();
    observore_census_verdict_t local  = ladder(v.local_mask, v.local_addrs,
                                               v.local_over);
    observore_census_verdict_t merged = ladder(v.mask, v.addrs, v.over);
    if (why) {
        memset(why, 0, sizeof(*why));
        why->local    = local;
        why->merged   = merged;
        why->via_mesh = local == OBSERVORE_CENSUS_REPORT &&
                        merged != OBSERVORE_CENSUS_REPORT;
        why->from_n   = v.from_n;
        memcpy(why->from, v.from, sizeof(why->from));
    }
    return merged;
}

observore_census_verdict_t observore_census_verdict(uint32_t id, int day,
                                                    observore_class_t cls)
{
    /* The same rule as observore_census_explain(), not a second copy of it, so
     * the score and the reason given for the score cannot come apart. The
     * protected-class check stays the first statement here as well. */
    if (observore_mute_class_is_protected(cls)) {
        return OBSERVORE_CENSUS_REPORT;
    }
    return observore_census_explain(id, day, cls, NULL);
}

void observore_census_counts(int day, int *household, int *tracked)
{
    CENSUS_LOCK();
    if (tracked) {
        *tracked = (int)s_count;
    }
    if (household) {
        int n = 0;
        for (size_t i = 0; i < s_count; i++) {
            if (popcount16(window_at(&s_tab[i], day)) >=
                OBSERVORE_CENSUS_MIN_DAYS) {
                n++;
            }
        }
        *household = n;
    }
    CENSUS_UNLOCK();
}

void observore_census_addr_spread(int *one, int *few, int *many)
{
    int a = 0, b = 0, c = 0;
    CENSUS_LOCK();
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
    CENSUS_UNLOCK();
    if (one)  { *one = a; }
    if (few)  { *few = b; }
    if (many) { *many = c; }
}

int observore_census_household_at_ceiling(int day)
{
    int n = 0;
    CENSUS_LOCK();
    for (size_t i = 0; i < s_count; i++) {
        bool at_cap = s_tab[i].addr_over ||
                      s_tab[i].addr_n >= OBSERVORE_CENSUS_ADDRS;
        if (at_cap && popcount16(window_at(&s_tab[i], day)) >=
                          OBSERVORE_CENSUS_MIN_DAYS) {
            n++;
        }
    }
    CENSUS_UNLOCK();
    return n;
}

bool observore_census_row(size_t i, int day, observore_census_row_t *out)
{
    if (!out) {
        return false;
    }
    bool ok = false;
    uint32_t id = 0;
    census_view_t v = {0};
    CENSUS_LOCK();
    if (i < s_count) {
        id = s_tab[i].id;
        ok = true;
    }
#if CONFIG_OBSERVORE_MESH_CENSUS
    else {
        size_t skip = i - s_count;
        for (size_t k = 0; k < p_count && !ok; k++) {
            if (find(p_tab[k].id)) {
                continue;
            }
            if (skip == 0) {
                id = p_tab[k].id;
                ok = true;
            } else {
                skip--;
            }
        }
    }
#endif
    if (ok) {
        view_of(id, day, &v);
    }
    CENSUS_UNLOCK();
    if (!ok) {
        return false;
    }
    /* From the same view the verdict is judged on, so a row and a verdict
     * cannot disagree about what is household. */
    memset(out, 0, sizeof(*out));
    out->id              = id;
    out->days            = popcount16(v.mask);
    out->local_days      = popcount16(v.local_mask);
    out->household       = out->days >= OBSERVORE_CENSUS_MIN_DAYS;
    out->household_local = out->local_days >= OBSERVORE_CENSUS_MIN_DAYS;
    out->via_mesh        = out->household && !out->household_local;
    out->here            = v.here;
    out->addrs           = v.addrs;
    out->more            = v.over;
    out->last_day        = v.last_day;
    out->from_n          = v.from_n;
    memcpy(out->from, v.from, sizeof(out->from));
    return true;
}

size_t observore_census_blob(void *out, size_t cap)
{
    CENSUS_LOCK();
    size_t need = sizeof(census_blob_hdr_t) +
                  s_count * sizeof(observore_census_entry_t);
    if (out && cap >= need) {
        census_blob_hdr_t hdr = {OBSERVORE_CENSUS_FORMAT, (uint16_t)s_count};
        memcpy(out, &hdr, sizeof(hdr));
        memcpy((char *)out + sizeof(hdr), s_tab,
               s_count * sizeof(observore_census_entry_t));
    }
    CENSUS_UNLOCK();
    return need;                  /* with no room, how much it wants */
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
    CENSUS_LOCK();
    if (src != (const void *)s_tab) {
        memcpy(s_tab, src, (size_t)hdr.count * sizeof(observore_census_entry_t));
    }
    s_count = hdr.count;
    CENSUS_UNLOCK();
    return true;
}

size_t observore_census_band_text(bool sharing, unsigned remaining_s,
                                  const char *level, unsigned score,
                                  char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    int n = snprintf(buf, len, " CENSUS %s %us  %s %u",
                     sharing ? "SHARE" : "LISTEN", remaining_s,
                     level ? level : "?", score);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

size_t observore_census_quiet_text(int quieted, int quieted_mesh, char *buf,
                                   size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    if (quieted <= 0) {
        buf[0] = '\0';
        return 0;
    }
    int n;
    if (quieted_mesh > 0) {
        n = snprintf(buf, len, "%d quiet, %d via mesh", quieted, quieted_mesh);
    } else {
        n = snprintf(buf, len, "%d quiet", quieted);
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

#if CONFIG_OBSERVORE_MESH_CENSUS
size_t observore_census_peer_count(void)
{
    CENSUS_LOCK();
    size_t n = p_count;
    CENSUS_UNLOCK();
    return n;
}

/* How many distinct days the owner's other nodes between them have on an
 * identity, as of `day`. A friend one day ahead is read in its own day, so a
 * day it has not yet reached here is still a day it has. */
static int peer_days(const observore_peer_census_entry_t *p, int day)
{
    int a = day > (int)p->last_day ? day : (int)p->last_day;
    uint16_t mask = 0;
    for (int k = 0; k < OBSERVORE_CENSUS_FROM; k++) {
        if (p->from[k].node) {
            mask |= align_mask(p->from[k].days, p->last_day, a);
        }
    }
    return popcount16(mask);
}

/* Which entry gives up its place when the peer table is full and a new
 * identity arrives, or NULL when the newcomer ranks below every resident.
 *
 * The same ranking as evict() above, for the same reason: fewest days first,
 * and the older of those, since the date alone ranks nothing on a stationary
 * board where every entry carries today's. There is no household tier here,
 * because what a friend reported is only worth keeping for the days it adds;
 * an entry three days deep is simply the one with the most to lose.
 *
 * Nothing in this function or its caller touches s_store. That is the property
 * the owner was promised: a friend, however many identities it reports, can
 * only ever cost another friend's report, never an identity this node learned
 * itself. */
static observore_peer_census_entry_t *peer_evict(int day, int newcomer_days)
{
    observore_peer_census_entry_t *worst = NULL;
    int worst_days = 0;
    for (size_t i = 0; i < p_count; i++) {
        observore_peer_census_entry_t *c = &p_tab[i];
        int days = peer_days(c, day);
        if (!worst || days < worst_days ||
            (days == worst_days && c->last_day < worst->last_day)) {
            worst = c;
            worst_days = days;
        }
    }
    if (!worst || worst_days > newcomer_days) {
        return NULL;
    }
    return worst;
}

/* Put an entry's contributors in node order, free places last, and its
 * addresses in value order. The order a friend's entries arrive in is not
 * something the owner controls, and nothing downstream depends on it, so the
 * stored form is made the same whichever came first: a re-delivery in another
 * order then compares equal and costs no flash write. */
static void peer_canon(observore_peer_census_entry_t *p)
{
    for (int i = 1; i < OBSERVORE_CENSUS_FROM; i++) {
        for (int j = i; j > 0; j--) {
            uint32_t a = p->from[j - 1].node ? p->from[j - 1].node : 0x10000u;
            uint32_t b = p->from[j].node ? p->from[j].node : 0x10000u;
            if (a <= b) {
                break;
            }
            uint16_t tn = p->from[j - 1].node;
            uint16_t td = p->from[j - 1].days;
            p->from[j - 1].node = p->from[j].node;
            p->from[j - 1].days = p->from[j].days;
            p->from[j].node = tn;
            p->from[j].days = td;
        }
    }
    for (int i = 1; i < p->addr_n; i++) {
        uint16_t key = p->addr[i];
        int j = i;
        while (j > 0 && p->addr[j - 1] > key) {
            p->addr[j] = p->addr[j - 1];
            j--;
        }
        p->addr[j] = key;
    }
}

/* Add one friend's entry to the stored entry `p`: the days into that friend's
 * place, the addresses into the union. Called under the lock, with `p` either
 * found or freshly emptied for this identity. */
static void peer_take(observore_peer_census_entry_t *p, uint16_t node,
                      int my_day, const observore_census_entry_t *in)
{
    /* Everything is held in the later of the two days, so that one shift rule
     * moves every contributor's mask and the incoming one onto the same bit. */
    int l = (int)p->last_day > (int)in->last_day ? (int)p->last_day
                                                 : (int)in->last_day;
    for (int k = 0; k < OBSERVORE_CENSUS_FROM; k++) {
        if (p->from[k].node) {
            p->from[k].days = align_mask(p->from[k].days, p->last_day, l);
        }
    }
    p->last_day = (uint16_t)l;

    /* This friend's own place if it has one, else a free place or one whose
     * friend has nothing left inside the window: that friend has no claim on
     * it any more. When there is none the friend's days are left out, which
     * only ever makes an identity less household. */
    int read_day = my_day > l ? my_day : l;
    int slot = -1;
    for (int k = 0; k < OBSERVORE_CENSUS_FROM && slot < 0; k++) {
        if (p->from[k].node == node) {
            slot = k;
        }
    }
    for (int k = 0; k < OBSERVORE_CENSUS_FROM && slot < 0; k++) {
        if (p->from[k].node == 0 ||
            align_mask(p->from[k].days, l, read_day) == 0) {
            slot = k;
        }
    }
    if (slot < 0) {
        /* No place is free and none is stale. The newcomer takes the place of
         * the friend with the fewest days inside the window if it brings
         * strictly more, and otherwise its days are dropped, so a tie keeps
         * the friend already counted. Dropping days only ever makes an
         * identity less household, which is the safe direction. */
        int worst = 0, worst_days = OBSERVORE_CENSUS_WINDOW + 1;
        for (int k = 0; k < OBSERVORE_CENSUS_FROM; k++) {
            int d = popcount16(align_mask(p->from[k].days, l, read_day));
            if (d < worst_days) {
                worst = k;
                worst_days = d;
            }
        }
        if (popcount16(align_mask(in->days, in->last_day, read_day)) > worst_days) {
            slot = worst;
        }
    }
    if (slot >= 0) {
        if (p->from[slot].node != node) {
            p->from[slot].node = node;
            p->from[slot].days = 0;
        }
        /* An OR and never a replace. A repeated, replayed or reordered entry
         * can only re-deliver days that already happened, so the table never
         * loses a day and never invents one. */
        p->from[slot].days |= align_mask(in->days, in->last_day, l);
    }
    for (uint8_t i = 0; i < in->addr_n; i++) {
        note_hash(p->addr, &p->addr_n, &p->addr_over, in->addr[i]);
    }
    if (in->addr_over) {
        p->addr_over = 1;
    }
    peer_canon(p);
}

observore_census_merge_t observore_census_peer_merge(uint16_t node, int my_day,
                                                     const observore_census_entry_t *in)
{
    if (!in || node == 0 || in->id == 0 ||
        my_day < 0 || my_day > OBSERVORE_CENSUS_DAY_MAX ||
        in->addr_n > OBSERVORE_CENSUS_ADDRS || in->addr_over > 1 ||
        in->days == 0 || (int)in->last_day > my_day + 1) {
        return OBSERVORE_CENSUS_MERGE_REFUSED;
    }
    /* Days that left the window before this node's day have nothing to add,
     * and are not worth a place in the table. A friend one day ahead is read
     * in its own day, so its tomorrow is kept for tomorrow. */
    int span = my_day > (int)in->last_day ? my_day : (int)in->last_day;
    if (align_mask(in->days, in->last_day, span) == 0) {
        return OBSERVORE_CENSUS_MERGE_SAME;
    }

    observore_census_merge_t result = OBSERVORE_CENSUS_MERGE_SAME;
    observore_peer_census_entry_t before = {0};
    CENSUS_LOCK();
    observore_peer_census_entry_t *p = find_peer(in->id);
    if (p) {
        before = *p;
    } else {
        if (p_count < OBSERVORE_PEER_CENSUS_MAX) {
            p = &p_tab[p_count++];
        } else {
            p = peer_evict(my_day,
                           popcount16(align_mask(in->days, in->last_day, span)));
        }
        if (p) {
            memset(p, 0, sizeof(*p));
            p->id       = in->id;
            p->last_day = in->last_day;
        }
    }
    if (!p) {
        result = OBSERVORE_CENSUS_MERGE_REFUSED;
    } else {
        peer_take(p, node, my_day, in);
        if (memcmp(&before, p, sizeof(before)) != 0) {
            s_peer_changed = true;
            result = OBSERVORE_CENSUS_MERGE_CHANGED;
        }
    }
    CENSUS_UNLOCK();
    return result;
}

void observore_census_peer_prune(int day)
{
    if (day < 0 || day > OBSERVORE_CENSUS_DAY_MAX) {
        return;           /* with no day nothing is known to have aged out */
    }
    CENSUS_LOCK();
    size_t kept = 0;
    for (size_t i = 0; i < p_count; i++) {
        if (peer_days(&p_tab[i], day) > 0) {
            if (kept != i) {
                p_tab[kept] = p_tab[i];
            }
            kept++;
        }
    }
    if (kept != p_count) {
        memset(&p_tab[kept], 0, (p_count - kept) * sizeof(p_tab[0]));
        p_count = (uint16_t)kept;
        s_peer_changed = true;
    }
    CENSUS_UNLOCK();
}

void observore_census_peer_forget(observore_census_forget_t why)
{
    CENSUS_LOCK();
    if (p_count > 0) {
        s_peer_changed = true;
    }
    p_count = 0;
    memset(p_tab, 0, sizeof(p_tab));
    s_peer_forgot = why;
    CENSUS_UNLOCK();
}

observore_census_forget_t observore_census_peer_forgot(void)
{
    CENSUS_LOCK();
    observore_census_forget_t why = s_peer_forgot;
    CENSUS_UNLOCK();
    return why;
}

bool observore_census_peer_take_changed(void)
{
    CENSUS_LOCK();
    bool was = s_peer_changed;
    s_peer_changed = false;
    CENSUS_UNLOCK();
    return was;
}

size_t observore_census_peer_blob(void *out, size_t cap)
{
    CENSUS_LOCK();
    size_t need = sizeof(census_peer_hdr_t) +
                  (size_t)p_count * sizeof(observore_peer_census_entry_t);
    if (out && cap >= need) {
        census_peer_hdr_t hdr = s_peer.hdr;
        hdr.version = OBSERVORE_PEER_CENSUS_FORMAT;
        memcpy(out, &hdr, sizeof(hdr));
        memcpy((char *)out + sizeof(hdr), p_tab,
               (size_t)p_count * sizeof(observore_peer_census_entry_t));
    }
    CENSUS_UNLOCK();
    return need;                  /* with no room, how much it wants */
}

/* Whether two key checks are the same, in time that does not depend on where
 * they first differ. It is four bytes of an HMAC of the household secret, so it
 * would confirm a guess at the secret offline: compared here, never logged and
 * never served. */
static bool check_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < OBSERVORE_PEER_CENSUS_CHECK_LEN; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* An empty table that carries the key check it is now held under. */
static void peer_empty(const uint8_t *check)
{
    p_count = 0;
    memset(p_tab, 0, sizeof(p_tab));
    s_peer.hdr.version = OBSERVORE_PEER_CENSUS_FORMAT;
    memcpy(s_peer.hdr.check, check, OBSERVORE_PEER_CENSUS_CHECK_LEN);
}

bool observore_census_peer_restore(const void *blob, size_t len,
                                   const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN])
{
    if (!check) {
        return false;
    }
    census_peer_hdr_t hdr = {0};
    bool shaped = blob && len >= sizeof(hdr);
    if (shaped) {
        memcpy(&hdr, blob, sizeof(hdr));
        /* The same refusals as the local table's. Another version, a count
         * past the table and a length that disagrees with the header are not
         * a table this code wrote, and a table that is half read is worse
         * than an empty one, so the peer table is emptied rather than left as
         * it was. */
        shaped = hdr.version == OBSERVORE_PEER_CENSUS_FORMAT &&
                 hdr.count <= OBSERVORE_PEER_CENSUS_MAX &&
                 len == sizeof(hdr) +
                        (size_t)hdr.count * sizeof(observore_peer_census_entry_t);
    }
    /* A table kept under another household secret is another household's
     * report. It is not read, and the owner is told why it is gone. */
    bool keyed = shaped && check_equal(hdr.check, check);
    bool ok = false;
    CENSUS_LOCK();
    if (keyed) {
        const void *src = (const char *)blob + sizeof(hdr);
        if (src != (const void *)p_tab) {
            memcpy(p_tab, src,
                   (size_t)hdr.count * sizeof(observore_peer_census_entry_t));
        }
        p_count = hdr.count;
        s_peer.hdr.version = OBSERVORE_PEER_CENSUS_FORMAT;
        memcpy(s_peer.hdr.check, check, OBSERVORE_PEER_CENSUS_CHECK_LEN);
        ok = true;
    } else {
        peer_empty(check);
        if (shaped) {
            s_peer_forgot  = OBSERVORE_CENSUS_FORGOT_SECRET;
            s_peer_changed = true;
        }
    }
    CENSUS_UNLOCK();
    return ok;
}

bool observore_census_peer_rekey(const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN])
{
    if (!check) {
        return false;
    }
    bool emptied = false;
    CENSUS_LOCK();
    if (!check_equal(s_peer.hdr.check, check)) {
        /* Nothing was kept under the old secret when the table is empty, so
         * there is nothing to say about it; the new check is stored either
         * way. */
        emptied = p_count > 0;
        peer_empty(check);
        if (emptied) {
            s_peer_forgot  = OBSERVORE_CENSUS_FORGOT_SECRET;
            s_peer_changed = true;
        }
    }
    CENSUS_UNLOCK();
    return emptied;
}

size_t observore_census_local_sendable(int day, observore_census_entry_t *out, size_t max)
{
    if (!out || max == 0 || day < 0 || day > OBSERVORE_CENSUS_DAY_MAX) {
        return 0;
    }
    size_t n = 0;
    CENSUS_LOCK();
    for (size_t i = 0; i < s_count && n < max; i++) {
        uint16_t mask = align_mask(s_tab[i].days, s_tab[i].last_day, day);
        if (mask == 0) {
            continue;                 /* every day it was seen has aged out */
        }
        out[n] = s_tab[i];
        out[n].days = mask;
        out[n].last_day = (uint16_t)day;
        n++;
    }
    CENSUS_UNLOCK();
    return n;
}
#endif /* CONFIG_OBSERVORE_MESH_CENSUS */

#ifndef OBSERVORE_HOST_TEST
#if CONFIG_OBSERVORE_MESH_CENSUS
void observore_census_peer_save(void)
{
    /* No copy: the header already sits in front of the entries, as it does for
     * the local census. */
    CENSUS_LOCK();
    s_peer.hdr.version = OBSERVORE_PEER_CENSUS_FORMAT;
    const observore_nvs_item_t item = {
        .key = "peercensus", .type = OBSERVORE_NVS_BLOB, .buf = &s_peer,
        .len = sizeof(census_peer_hdr_t) +
               (size_t)p_count * sizeof(observore_peer_census_entry_t)};
    esp_err_t err = observore_nvs_write(&item, 1);
    CENSUS_UNLOCK();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save the peer census");
    }
}

void observore_census_peer_load(const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN],
                                bool have_secret)
{
    /* Straight into the store, as the local census is read: a blob that does
     * not validate empties the table, so there is nothing to protect by reading
     * somewhere else first. */
    observore_nvs_item_t item = {.key = "peercensus", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &s_peer, .len = sizeof(s_peer)};
    observore_nvs_read(&item, 1);
    const bool stored = item.found && item.len > sizeof(census_peer_hdr_t);

    if (!have_secret) {
        /* A table with no household secret behind it is nobody's report. It is
         * read only to know whether flash still holds one: emptying RAM and
         * leaving the old table in flash would bring it back under the next
         * secret that happened to hash alike, or simply keep what the owner
         * cleared. */
        static const uint8_t none[OBSERVORE_PEER_CENSUS_CHECK_LEN] = {0};
        observore_census_peer_restore(NULL, 0, none);
        observore_census_peer_forget(OBSERVORE_CENSUS_FORGOT_NO_SECRET);
        if (stored) {
            ESP_LOGI(TAG, "the saved peer census was kept without a household "
                          "secret -- it starts empty");
            observore_census_peer_save();
        }
        observore_census_peer_take_changed();
        return;
    }
    if (!item.found) {
        observore_census_peer_restore(NULL, 0, check);   /* empty, holding the check */
        return;
    }
    if (observore_census_peer_restore(&s_peer, item.len, check)) {
        if (observore_census_peer_count() > 0) {
            ESP_LOGI(TAG, "peer census: %u identities from your other nodes",
                     (unsigned)observore_census_peer_count());
        }
        return;
    }
    /* A refusal for the key is the only one that marks the table changed, so
     * that is how the two reasons are told apart. */
    if (observore_census_peer_take_changed()) {
        ESP_LOGW(TAG, "the household secret changed since the peer census was "
                      "saved -- it starts empty");
    } else {
        ESP_LOGW(TAG, "the saved peer census was written by an older build "
                      "(%u bytes) -- starting empty", (unsigned)item.len);
    }
    observore_census_peer_save();
}
#endif /* CONFIG_OBSERVORE_MESH_CENSUS */

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
