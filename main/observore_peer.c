#include "observore_peer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "observore_detect.h"
#include "observore_types.h"

/* The table is written from the BLE host task and read from the main loop, the
 * HTTP server and the display, and all of those run on two cores. A reader
 * that catches a warning half-written could pair a stranger's class with a
 * friend's trust flag for one pass, which matters now that trust carries
 * score. So every access takes a recursive lock, the same arrangement as the
 * device table. The host build has no tasks and needs no lock, which keeps
 * this file free of RTOS includes there.
 *
 * Lock order is the tracker's lock first, then this one; nothing in this file
 * calls into the tracker. Every critical section is a bounded loop over at
 * most OBSERVORE_PEER_MAX entries and a struct copy. */
#ifdef OBSERVORE_HOST_TEST
#define PEER_READY()  1
#define PEER_LOCK()   do {} while (0)
#define PEER_UNLOCK() do {} while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
/* Before observore_peer_init() has run there is no lock to take, and an empty
 * answer is the right one: no warnings have been heard. */
#define PEER_READY()  (s_lock != NULL)
#define PEER_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define PEER_UNLOCK() xSemaphoreGiveRecursive(s_lock)
#endif

#define FLAG_POSITION 0x01
#define FLAG_TAG      0x02

#define BASE_LEN      12    /* magic through age, with nothing optional */
#define POSITION_LEN   6    /* three bytes a coordinate */

/* Coarse positions: degrees times ten million, divided by 256, in three
 * signed bytes. About 2.8 metres. A power of two so the arithmetic is exact
 * both ways and there is no rounding decision to get wrong. */
#define COARSE_SHIFT 256

static observore_peer_warning_t s_tab[OBSERVORE_PEER_MAX];
static size_t s_count;

/* The record of this boot's peer reports, oldest first so a new event is
 * appended and "newest first" is a reverse walk with no tie to break. Touched
 * only under PEER_LOCK. */
static observore_peer_event_t s_events[OBSERVORE_PEER_EVENTS_MAX];
static size_t s_event_n;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Three bytes, little-endian, sign-extended. Written out rather than shifted
 * into place from a 32-bit load, because the top byte of a 24-bit field is a
 * sign bit and sign-extension by shifting right is implementation-defined. */
static int32_t le24s(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (v & 0x800000u) {
        v |= 0xFF000000u;
    }
    return (int32_t)v;
}

/* The same rule the drone decoder applies, for the same reason: zero/zero is
 * a real place in the Gulf of Guinea, and a node that has no fix sends
 * zeroes. Repeating it as a position would be a confident lie. */
bool observore_peer_position_usable(int32_t lat_e7, int32_t lon_e7)
{
    if (lat_e7 == 0 && lon_e7 == 0) {
        return false;
    }
    return lat_e7 <= 900000000 && lat_e7 >= -900000000 &&
           lon_e7 <= 1800000000 && lon_e7 >= -1800000000;
}

bool observore_peer_parse(const uint8_t *payload, size_t len,
                          observore_peer_warning_t *out)
{
    if (!payload || !out || len < BASE_LEN) {
        return false;
    }
    if (memcmp(payload, OBSERVORE_PEER_MAGIC, 4) != 0) {
        return false;
    }
    if (payload[4] != OBSERVORE_PEER_VERSION) {
        return false;      /* our own format; a layout we no longer use is not guessed at */
    }

    uint8_t flags = payload[5];
    size_t need = BASE_LEN;
    if (flags & FLAG_POSITION) { need += POSITION_LEN; }
    if (flags & FLAG_TAG)      { need += OBSERVORE_PEER_TAG_LEN; }
    if (len < need) {
        return false;      /* it claims more than arrived */
    }

    memset(out, 0, sizeof(*out));
    out->node  = le16(&payload[6]);
    out->cls   = payload[8];
    out->seq   = le16(&payload[9]);
    out->age_s = payload[11];

    /* A class this build does not have is a node that knows about something
     * we do not. Kept as unknown rather than dropped: that a neighbour is
     * warning at all is the useful part, and inventing a class number we
     * cannot name would be worse than admitting we cannot name it. */
    if (out->cls >= OBSERVORE_CLASS_MAX) {
        out->cls = OBSERVORE_CLASS_UNKNOWN;
    }

    if (flags & FLAG_POSITION) {
        int32_t lat = le24s(&payload[12]) * COARSE_SHIFT;
        int32_t lon = le24s(&payload[15]) * COARSE_SHIFT;
        if (observore_peer_position_usable(lat, lon)) {
            out->lat_e7 = lat;
            out->lon_e7 = lon;
            out->have_pos = true;
        }
    }

    /* A tag is parsed for its length and no more, and `trusted` is always
     * left false here. Whether a tag is genuine is decided at the receive
     * site, observore_ble.c, which asks observore_meshkey_verify() to check
     * it over the span observore_peer_tag_span() returns and only then sets
     * the flag. The parser is pure and host-tested and has no key, so it can
     * never be the thing that vouches for a warning: a tag nobody has checked
     * must not be mistaken for one that has been. */
    out->trusted = false;
    return true;
}

/* Manufacturer-specific data, and the SIG's reserved non-production company
 * ID. Both are shared with every other hobby project that made the same
 * honest choice, which is why the magic is what makes a warning ours. */
#define AD_TYPE_MFG_DATA      0xFF
#define COMPANY_NONPRODUCTION 0xFFFF

bool observore_peer_from_advert(const uint8_t *adv, size_t adv_len,
                               observore_peer_warning_t *out)
{
    size_t mfg_len = 0;
    const uint8_t *mfg = observore_adv_field(adv, adv_len, AD_TYPE_MFG_DATA,
                                             &mfg_len);
    if (!mfg || mfg_len < 2) {
        return false;
    }
    uint16_t company = (uint16_t)(mfg[0] | ((uint16_t)mfg[1] << 8));
    if (company != COMPANY_NONPRODUCTION) {
        return false;
    }
    return observore_peer_parse(mfg + 2, mfg_len - 2, out);
}

uint16_t observore_peer_node_id(const uint8_t mac[6])
{
    if (!mac) {
        return 1;
    }
    /* FNV-1a folded to sixteen bits, with the basis nudged so this cannot
     * collide with the census's address hash by being the same arithmetic
     * over the same bytes. */
    uint32_t h = 0x811C9DC5u ^ 0x4E4F4445u;      /* "NODE" */
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    uint16_t id = (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
    return id ? id : 1u;     /* zero reads as "no id set" */
}

static void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* Three signed bytes, little-endian, of degrees-times-ten-million divided by
 * 256. The division is arithmetic rather than a shift: shifting a negative
 * value right is implementation-defined, and the whole point of a power-of-two
 * divisor is that the arithmetic is exact in both directions. */
static void le24s_put(uint8_t *p, int32_t e7)
{
    int32_t v = e7 / COARSE_SHIFT;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

size_t observore_peer_build_tagged(const observore_peer_warning_t *w,
                                   const uint8_t *tag,
                                   uint8_t *out, size_t cap)
{
    if (!w || !out) {
        return 0;
    }
    bool pos = w->have_pos &&
               observore_peer_position_usable(w->lat_e7, w->lon_e7);
    size_t need = BASE_LEN + (pos ? POSITION_LEN : 0) +
                  (tag ? OBSERVORE_PEER_TAG_LEN : 0);
    if (cap < need) {
        return 0;
    }

    memcpy(out, OBSERVORE_PEER_MAGIC, 4);
    out[4] = OBSERVORE_PEER_VERSION;
    out[5] = (uint8_t)((pos ? FLAG_POSITION : 0) | (tag ? FLAG_TAG : 0));
    le16_put(&out[6], w->node);
    /* A class this build does not have cannot be described, so it is sent as
     * unknown rather than as a number a receiver would read as some other
     * class entirely. */
    out[8] = (w->cls < OBSERVORE_CLASS_MAX) ? w->cls : OBSERVORE_CLASS_UNKNOWN;
    le16_put(&out[9], w->seq);
    /* One byte, and warnings expire at five minutes, so anything older than
     * the field can hold is clamped rather than wrapped: 255 seconds reads as
     * "a while ago", where a wrap would read as "just now". */
    out[11] = (w->age_s > 255) ? 255 : (uint8_t)w->age_s;

    if (pos) {
        le24s_put(&out[12], w->lat_e7);
        le24s_put(&out[15], w->lon_e7);
    }
    /* Last, over everything before it -- which is what the span returned to a
     * verifier describes. */
    if (tag) {
        memcpy(out + need - OBSERVORE_PEER_TAG_LEN, tag, OBSERVORE_PEER_TAG_LEN);
    }
    return need;
}

size_t observore_peer_build(const observore_peer_warning_t *w,
                            uint8_t *out, size_t cap)
{
    return observore_peer_build_tagged(w, NULL, out, cap);
}

size_t observore_peer_tag_span(const uint8_t *payload, size_t len,
                               const uint8_t **tag)
{
    if (tag) {
        *tag = NULL;
    }
    if (!payload || len < BASE_LEN) {
        return 0;
    }
    if (memcmp(payload, OBSERVORE_PEER_MAGIC, 4) != 0 ||
        payload[4] != OBSERVORE_PEER_VERSION) {
        return 0;
    }
    uint8_t flags = payload[5];
    if (!(flags & FLAG_TAG)) {
        return 0;
    }
    size_t span = BASE_LEN + ((flags & FLAG_POSITION) ? POSITION_LEN : 0);
    /* A payload claiming a tag that did not arrive gets no span: verifying
     * against bytes past the end is the mistake this exists to prevent. */
    if (len < span + OBSERVORE_PEER_TAG_LEN) {
        return 0;
    }
    if (tag) {
        *tag = payload + span;
    }
    return span;
}

size_t observore_peer_advert_tagged(const observore_peer_warning_t *w,
                                    const uint8_t *tag,
                                    uint8_t *out, size_t cap)
{
    /* length, type, company low, company high, then the payload. The length
     * byte counts everything after itself, which is the one part of an AD
     * element that is easy to get wrong by one. */
    if (!out || cap < 4) {
        return 0;
    }
    size_t n = observore_peer_build_tagged(w, tag, out + 4, cap - 4);
    if (n == 0) {
        return 0;
    }
    out[0] = (uint8_t)(1 + 2 + n);          /* type + company + payload */
    out[1] = AD_TYPE_MFG_DATA;
    le16_put(&out[2], COMPANY_NONPRODUCTION);
    return n + 4;
}

void observore_peer_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    /* Once, at boot, before the BLE task starts; the tests call this many
     * times and the mutex must not be recreated under a task that holds it. */
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_lock) {
        return;
    }
#endif
    PEER_LOCK();
    s_count = 0;
    memset(s_tab, 0, sizeof(s_tab));
    s_event_n = 0;
    memset(s_events, 0, sizeof(s_events));
    PEER_UNLOCK();
}

static bool expired(const observore_peer_warning_t *w, int64_t now_us)
{
    return (now_us - w->heard_us) > (int64_t)OBSERVORE_PEER_TTL_S * 1000000;
}

/* Whether `seq` is ahead of `last`, modulo the sequence space. */
static bool seq_newer(uint16_t seq, uint16_t last)
{
    uint16_t ahead = (uint16_t)(seq - last);
    return ahead != 0 && ahead <= OBSERVORE_PEER_SEQ_WINDOW;
}

/* Which entry to give up when the table is full, or `s_count` for none.
 *
 * An expired entry first, of either kind, so a stale warning never costs a
 * live one its place. Then the oldest stranger: strangers are the cheap
 * thing to lose. A trusted entry is given up only to admit another trusted
 * warning, and a stranger never evicts a live trusted one -- nine invented
 * ids must not be able to push every friend out of the table. */
static size_t victim_for(const observore_peer_warning_t *w, int64_t now_us)
{
    size_t pick = s_count;
    for (size_t i = 0; i < s_count; i++) {
        if (expired(&s_tab[i], now_us) &&
            (pick == s_count || s_tab[i].heard_us < s_tab[pick].heard_us)) {
            pick = i;
        }
    }
    if (pick != s_count) {
        return pick;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (!s_tab[i].trusted &&
            (pick == s_count || s_tab[i].heard_us < s_tab[pick].heard_us)) {
            pick = i;
        }
    }
    if (pick != s_count || !w->trusted) {
        return pick;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (pick == s_count || s_tab[i].heard_us < s_tab[pick].heard_us) {
            pick = i;
        }
    }
    return pick;
}

/* Called with the lock held. */
static bool note_locked(const observore_peer_warning_t *w, int64_t now_us)
{
    /* One entry per node and per kind of trust: the latest thing a neighbour
     * said, not a log of everything it has ever said. A node repeating itself
     * must not be able to fill the table and push other nodes out, which is
     * the cheapest attack there is on a structure like this.
     *
     * Trust is part of the key because the node id is not authenticated: a
     * stranger can send a friend's id. Keyed on the id alone, that would
     * overwrite the friend's entry, or -- with a sequence number far ahead of
     * the friend's -- make every warning the friend sent next look like a
     * replay. Keyed on both, the stranger gets an entry and a sequence window
     * of its own and the friend's are untouched. A friend whose tag failed
     * once (a key mismatch, or a send that went out unsigned) is therefore one
     * node with two entries until the older one's hold ends. */
    for (size_t i = 0; i < s_count; i++) {
        if (s_tab[i].node != w->node || s_tab[i].trusted != w->trusted) {
            continue;
        }
        if (!expired(&s_tab[i], now_us) && !seq_newer(w->seq, s_tab[i].seq)) {
            return false;      /* a replay, or the same warning again */
        }
        s_tab[i] = *w;
        s_tab[i].heard_us = now_us;
        return true;
    }

    if (s_count < OBSERVORE_PEER_MAX) {
        s_tab[s_count] = *w;
        s_tab[s_count].heard_us = now_us;
        s_count++;
        return true;
    }

    /* Full. See victim_for() for who goes; a stranger who finds only live
     * friends is refused rather than let in. */
    size_t v = victim_for(w, now_us);
    if (v == s_count) {
        return false;
    }
    s_tab[v] = *w;
    s_tab[v].heard_us = now_us;
    return true;
}

bool observore_peer_note(const observore_peer_warning_t *w, int64_t now_us)
{
    if (!w || !PEER_READY()) {
        return false;
    }
    PEER_LOCK();
    bool kept = note_locked(w, now_us);
    PEER_UNLOCK();
    return kept;
}

size_t observore_peer_recent(observore_peer_warning_t *out, size_t max,
                             int64_t now_us)
{
    if (!out || max == 0 || !PEER_READY()) {
        return 0;
    }
    PEER_LOCK();
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        if (expired(&s_tab[i], now_us)) {
            continue;
        }
        /* Newest first, inserted in place: the table is eight entries, so
         * this is cheaper than sorting and keeps the caller's buffer bounded. */
        size_t at = n;
        while (at > 0 && out[at - 1].heard_us < s_tab[i].heard_us) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = s_tab[i];
        n++;
    }
    PEER_UNLOCK();
    return n;
}

int observore_peer_trusted(int64_t now_us)
{
    if (!PEER_READY()) {
        return 0;
    }
    PEER_LOCK();
    int n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (!expired(&s_tab[i], now_us) && s_tab[i].trusted) {
            n++;
        }
    }
    PEER_UNLOCK();
    return n;
}

void observore_peer_counts(int64_t now_us, int *nodes, int *warnings)
{
    int live = 0;
    int distinct = 0;
    if (PEER_READY()) {
        PEER_LOCK();
        for (size_t i = 0; i < s_count; i++) {
            if (expired(&s_tab[i], now_us)) {
                continue;
            }
            live++;
            /* Distinct ids: a friend heard both signed and unsigned is one
             * node with two standing warnings, and reporting it as two nodes
             * would overstate who is in range. */
            bool seen = false;
            for (size_t j = 0; j < i && !seen; j++) {
                seen = !expired(&s_tab[j], now_us) &&
                       s_tab[j].node == s_tab[i].node;
            }
            if (!seen) {
                distinct++;
            }
        }
        PEER_UNLOCK();
    }
    if (nodes)    { *nodes = distinct; }
    if (warnings) { *warnings = live; }
}

size_t observore_peer_advert(const observore_peer_warning_t *w,
                             uint8_t *out, size_t cap)
{
    return observore_peer_advert_tagged(w, NULL, out, cap);
}

const char *observore_peer_why_name(observore_peer_why_t why)
{
    switch (why) {
    case OBSERVORE_PEER_WHY_COUNTED:          return "counted";
    case OBSERVORE_PEER_WHY_STALE:            return "stale";
    case OBSERVORE_PEER_WHY_CLASS_NOT_SCORED: return "class not scored";
    case OBSERVORE_PEER_WHY_MONITOR_OFF:      return "monitor off here";
    case OBSERVORE_PEER_WHY_NOT_SEEN_HERE:    return "not seen here";
    case OBSERVORE_PEER_WHY_FOLDED:           return "folded";
    case OBSERVORE_PEER_WHY_LIMITED:          return "limited";
    }
    return "?";
}

/* Whether a warning about this class may add points at all.
 *
 * The line is the caution weight: a class that this node itself scores below
 * caution (a camera, fleet telematics, an accessory) is reported and barely
 * scored on purpose, and a neighbour's report of it must not out-weigh that.
 * Followers are unidentified by definition and capped for that reason, so the
 * mesh cannot be a way round the cap. Tailing is a fact about the sender's
 * own journey between places, which is not this node's journey, and it was
 * rebuilt three times to stop it alerting falsely; it stays advisory.
 *
 * A class added later defaults to ineligible unless it is weighted at caution
 * or more, which is the safe direction. Private on purpose: the only caller
 * is the rule below, and the eligible set is pinned through that rule. */
static bool peer_eligible(observore_class_t c)
{
    return (unsigned)c < OBSERVORE_CLASS_MAX &&
           observore_class_points(c) >= OBSERVORE_SCORE_CAUTION &&
           c != OBSERVORE_CLASS_FOLLOWER && c != OBSERVORE_CLASS_TAILING;
}

static unsigned umin(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

/* Room left under the alert line for a lone trusted voice, which is allowed to
 * lift the board to caution and no further. Zero once the number is already at
 * the top of caution or above. */
static unsigned alert_room(uint32_t running)
{
    return running < (uint32_t)(OBSERVORE_SCORE_ALERT - 1)
               ? (unsigned)(OBSERVORE_SCORE_ALERT - 1) - (unsigned)running
               : 0;
}

/* Room left in the level band the running number is already in, for evidence
 * that must not change the level at all: a stranger. Under caution that is the
 * space up to caution - 1, in caution the space up to alert - 1, and zero from
 * alert up. The bound belongs here rather than in whichever class table feeds
 * the base, so it holds whatever the local sum did to that base -- a device the
 * census dampened scores under its class weight, and the counting mask still
 * says the class is counted. */
static unsigned band_room(uint32_t running)
{
    if (running >= (uint32_t)OBSERVORE_SCORE_ALERT) {
        return 0;
    }
    uint32_t top = running >= (uint32_t)OBSERVORE_SCORE_CAUTION
                       ? (uint32_t)(OBSERVORE_SCORE_ALERT - 1)
                       : (uint32_t)(OBSERVORE_SCORE_CAUTION - 1);
    return (unsigned)(top - running);
}

/* Hand `alloc` points to the trusted senders of one class, lowest node id
 * first, each at most OBSERVORE_PEER_TRUSTED_POINTS. Lowest first so the same
 * set of warnings gives the same shares whichever order the table holds them
 * in. A sender that received its whole value is COUNTED; the rest stay
 * LIMITED, with whatever they got. */
static void credit_trusted(const observore_peer_warning_t *w, size_t n,
                           observore_peer_weight_t *out, const bool *cand,
                           unsigned cls, unsigned alloc, unsigned full)
{
    bool done[OBSERVORE_PEER_MAX] = {false};
    unsigned left = alloc;
    for (;;) {
        int pick = -1;
        for (size_t i = 0; i < n; i++) {
            if (!cand[i] || done[i] || !w[i].trusted || w[i].cls != cls) {
                continue;
            }
            if (pick < 0 || w[i].node < w[pick].node) {
                pick = (int)i;
            }
        }
        if (pick < 0) {
            return;
        }
        done[pick] = true;
        unsigned give = umin(OBSERVORE_PEER_TRUSTED_POINTS, left);
        left -= give;
        out->share[pick].points = (uint8_t)give;
        if (give == full) {
            out->share[pick].why = OBSERVORE_PEER_WHY_COUNTED;
        }
    }
}

void observore_peer_weigh(const observore_peer_warning_t *w, size_t nw,
                          uint32_t counting_mask, uint32_t monitors_off_mask,
                          uint16_t base_score, int64_t now_us,
                          observore_peer_weight_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!w) {
        nw = 0;
    }
    size_t n = nw < OBSERVORE_PEER_MAX ? nw : OBSERVORE_PEER_MAX;
    out->n = (uint8_t)n;

    bool cand[OBSERVORE_PEER_MAX] = {false};
    unsigned nt[OBSERVORE_CLASS_MAX] = {0};     /* trusted candidates      */
    unsigned ns[OBSERVORE_CLASS_MAX] = {0};     /* stranger candidates     */
    unsigned nsp[OBSERVORE_CLASS_MAX] = {0};    /* ... that gave a position */
    int rep[OBSERVORE_CLASS_MAX];               /* the one stranger voice  */
    unsigned spent[OBSERVORE_CLASS_MAX] = {0};  /* points given per class  */
    for (size_t c = 0; c < OBSERVORE_CLASS_MAX; c++) {
        rep[c] = -1;
    }

    for (size_t i = 0; i < n; i++) {
        observore_peer_share_t *s = &out->share[i];
        int64_t elapsed_us = now_us - w[i].heard_us;
        if (elapsed_us < 0) {
            elapsed_us = 0;
        }
        int64_t eff_s = (int64_t)w[i].age_s + elapsed_us / 1000000;

        s->node = w[i].node;
        s->cls = w[i].cls;
        s->trusted = w[i].trusted;
        s->seen_here = w[i].cls < OBSERVORE_CLASS_MAX &&
                       ((counting_mask >> w[i].cls) & 1u);
        s->voices = 1;
        s->age_s = (uint16_t)(eff_s > 65535 ? 65535 : eff_s);

        /* The sender's position, carried for display. Nothing below that
         * decides cand[], nt[], ns[], rep[], points or why reads these
         * fields, so a position, forged or genuine, cannot move a score. */
        s->have_pos = w[i].have_pos &&
                      observore_peer_position_usable(w[i].lat_e7, w[i].lon_e7);
        s->lat_e7 = s->have_pos ? w[i].lat_e7 : 0;
        s->lon_e7 = s->have_pos ? w[i].lon_e7 : 0;
        s->pos_n = s->have_pos ? 1 : 0;

        /* A cut-off and never a decay: the share is the full value until the
         * hold ends. Compared in microseconds so nothing rounds. */
        bool fresh = w[i].age_s < OBSERVORE_PEER_STALE_AGE_S &&
                     elapsed_us + (int64_t)w[i].age_s * 1000000 <
                         (int64_t)OBSERVORE_PEER_TTL_S * 1000000;
        if (!fresh) {
            s->why = OBSERVORE_PEER_WHY_STALE;
        } else if (!peer_eligible((observore_class_t)w[i].cls)) {
            s->why = OBSERVORE_PEER_WHY_CLASS_NOT_SCORED;
        } else if ((monitors_off_mask >> w[i].cls) & 1u) {
            /* A monitor switched off here stays off through the mesh. The
             * owner said stop reporting this class, and
             * observore_monitors.h promises that no automatic source, a mesh
             * peer included, overrides that -- so even a friend's warning
             * adds nothing, and is still listed so the silence is visible. */
            s->why = OBSERVORE_PEER_WHY_MONITOR_OFF;
        } else if (!w[i].trusted && !s->seen_here) {
            /* A stranger can add weight to what this node already sees and
             * can never put a class on the board alone. */
            s->why = OBSERVORE_PEER_WHY_NOT_SEEN_HERE;
        } else {
            cand[i] = true;
            s->why = OBSERVORE_PEER_WHY_LIMITED;
            if (w[i].trusted) {
                nt[w[i].cls]++;
            } else {
                ns[w[i].cls]++;
                if (s->have_pos) {
                    nsp[w[i].cls]++;
                }
                /* Strangers fold to one voice per class: the lowest node id,
                 * the lowest index on a tie, so the same warnings pick the
                 * same node whichever order they arrived in. */
                if (rep[w[i].cls] < 0 ||
                    w[i].node < w[rep[w[i].cls]].node) {
                    rep[w[i].cls] = (int)i;
                }
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (cand[i] && !w[i].trusted) {
            if ((int)i == rep[w[i].cls]) {
                out->share[i].voices = (uint8_t)ns[w[i].cls];
                out->share[i].pos_n = (uint8_t)nsp[w[i].cls];
            } else {
                out->share[i].why = OBSERVORE_PEER_WHY_FOLDED;
            }
        }
    }

    /* The order every pass spends the shared budgets in: heaviest class
     * first, then class id. Derived from the one class table. */
    uint8_t order[OBSERVORE_CLASS_MAX];
    size_t no = 0;
    for (int wt = OBSERVORE_PEER_CLASS_CAP; wt >= OBSERVORE_SCORE_CAUTION; wt--) {
        for (size_t c = 0; c < OBSERVORE_CLASS_MAX; c++) {
            if (peer_eligible((observore_class_t)c) &&
                umin(observore_class_points((observore_class_t)c),
                     OBSERVORE_PEER_CLASS_CAP) == (unsigned)wt) {
                order[no++] = (uint8_t)c;
            }
        }
    }

    uint32_t running = base_score;
    unsigned remaining = OBSERVORE_PEER_TOTAL_CAP;
    unsigned stranger_left = OBSERVORE_PEER_STRANGER_CAP;

    /* Pass 1: trusted voices that are allowed to reach alert -- two or more
     * friends agreeing, or one friend confirming a class that alerts on its
     * own here. Never more than the class is worth (cap_c). */
    for (size_t k = 0; k < no; k++) {
        unsigned c = order[k];
        unsigned cap_c = umin(OBSERVORE_PEER_CLASS_CAP,
                              observore_class_points((observore_class_t)c));
        bool seen = (counting_mask >> c) & 1u;
        if (nt[c] == 0 ||
            !(nt[c] >= 2 || (seen && cap_c >= OBSERVORE_SCORE_ALERT))) {
            continue;
        }
        unsigned alloc = umin(umin(nt[c] * OBSERVORE_PEER_TRUSTED_POINTS, cap_c),
                              remaining);
        credit_trusted(w, n, out, cand, c, alloc,
                       umin(OBSERVORE_PEER_TRUSTED_POINTS, cap_c));
        running += alloc;
        remaining -= alloc;
        spent[c] += alloc;
    }

    /* Pass 2: one trusted voice that must stay below alert -- about a class
     * this node does not see, or confirming one that is worth less than alert
     * by itself. It can lift the board to caution and no further. */
    for (size_t k = 0; k < no; k++) {
        unsigned c = order[k];
        unsigned cap_c = umin(OBSERVORE_PEER_CLASS_CAP,
                              observore_class_points((observore_class_t)c));
        bool seen = (counting_mask >> c) & 1u;
        if (nt[c] != 1 || (seen && cap_c >= OBSERVORE_SCORE_ALERT)) {
            continue;
        }
        unsigned alloc = umin(umin(OBSERVORE_PEER_TRUSTED_POINTS, cap_c),
                              umin(remaining, alert_room(running)));
        credit_trusted(w, n, out, cand, c, alloc,
                       umin(OBSERVORE_PEER_TRUSTED_POINTS, cap_c));
        running += alloc;
        remaining -= alloc;
        spent[c] += alloc;
    }

    /* Pass 3: strangers, one voice per class, under their own cap and the
     * room left in the level band the number is already in. They attach only
     * to classes counted here, and the band clamp means this can move the
     * number inside a level and can never change the level, from any base. */
    for (size_t k = 0; k < no; k++) {
        unsigned c = order[k];
        if (rep[c] < 0) {
            continue;
        }
        unsigned cap_c = umin(OBSERVORE_PEER_CLASS_CAP,
                              observore_class_points((observore_class_t)c));
        unsigned alloc = umin(umin(OBSERVORE_PEER_STRANGER_POINTS,
                                   cap_c - spent[c]),
                              umin(umin(remaining, stranger_left),
                                   band_room(running)));
        observore_peer_share_t *s = &out->share[rep[c]];
        s->points = (uint8_t)alloc;
        if (alloc == OBSERVORE_PEER_STRANGER_POINTS) {
            s->why = OBSERVORE_PEER_WHY_COUNTED;
        }
        running += alloc;
        remaining -= alloc;
        stranger_left -= alloc;
        spent[c] += alloc;
    }

    for (size_t i = 0; i < n; i++) {
        out->total = (uint8_t)(out->total + out->share[i].points);
        if (out->share[i].trusted) {
            out->trusted = (uint8_t)(out->trusted + out->share[i].points);
        } else {
            out->stranger = (uint8_t)(out->stranger + out->share[i].points);
        }
    }
}

void observore_peer_weigh_now(int64_t now_us, uint32_t counting_mask,
                              uint32_t monitors_off_mask, uint16_t base_score,
                              observore_peer_weight_t *out)
{
    if (!out) {
        return;
    }
    if (!PEER_READY()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    PEER_LOCK();
    observore_peer_weigh(s_tab, s_count, counting_mask, monitors_off_mask,
                         base_score, now_us, out);
    PEER_UNLOCK();
}

/* ---- the record of this boot's peer reports ------------------------------
 *
 * The ring itself is declared with the warning table, above. */

static bool event_matches(const observore_peer_event_t *e,
                          const observore_peer_share_t *s)
{
    if (e->share.cls != s->cls || e->share.trusted != s->trusted) {
        return false;
    }
    /* Every stranger about a class is one voice, whichever id speaks for it. */
    return !s->trusted || e->share.node == s->node;
}

/* The slot to give up, never one a share matched this round: strangers before
 * friends, closed before open, then oldest. Returns SIZE_MAX if every slot is
 * protected, which the sizes make impossible (at most six shares can count,
 * and the ring holds eight). */
static size_t event_victim(const bool *keep)
{
    size_t best = SIZE_MAX;
    int best_rank = 4;
    for (size_t i = 0; i < s_event_n; i++) {
        if (keep[i]) {
            continue;
        }
        const observore_peer_event_t *e = &s_events[i];
        int rank = e->share.trusted ? (e->open ? 3 : 2) : (e->open ? 1 : 0);
        if (rank < best_rank ||
            (rank == best_rank && e->last_us < s_events[best].last_us)) {
            best = i;
            best_rank = rank;
        }
    }
    return best;
}

size_t observore_peer_events_update(const observore_peer_weight_t *pw,
                                    int64_t now_us, uint8_t *started,
                                    size_t max_started)
{
    if (!pw || !PEER_READY()) {
        return 0;
    }
    bool keep[OBSERVORE_PEER_EVENTS_MAX] = { false };
    bool taken[OBSERVORE_PEER_MAX] = { false };
    size_t written = 0;

    PEER_LOCK();
    /* Existing events first: a share that is still counting refreshes its own
     * event, and an open event nothing matched stops here. */
    for (size_t k = 0; k < pw->n && k < OBSERVORE_PEER_MAX; k++) {
        const observore_peer_share_t *s = &pw->share[k];
        if (s->points == 0) {
            continue;
        }
        for (size_t i = 0; i < s_event_n; i++) {
            if (s_events[i].open && !keep[i] && event_matches(&s_events[i], s)) {
                uint16_t node = s_events[i].share.node;
                s_events[i].share = *s;
                s_events[i].share.node = node;   /* the one the log named */
                /* A stranger event is keyed on the class and keeps the node
                 * its log line named, while the refreshed position belongs to
                 * whichever stranger now speaks for the class. Showing it
                 * under the old id would attach a position to a node that
                 * never sent it, so it is dropped until the named node
                 * speaks again. A trusted event is keyed on its node, so its
                 * whole share, position included, always follows the newest
                 * warning. */
                if (!s->trusted && s->node != node) {
                    s_events[i].share.have_pos = false;
                    s_events[i].share.lat_e7 = 0;
                    s_events[i].share.lon_e7 = 0;
                }
                s_events[i].last_us = now_us;
                keep[i] = true;
                taken[k] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < s_event_n; i++) {
        if (!keep[i]) {
            s_events[i].open = false;
        }
    }
    /* What is left counting is new. */
    for (size_t k = 0; k < pw->n && k < OBSERVORE_PEER_MAX; k++) {
        const observore_peer_share_t *s = &pw->share[k];
        if (s->points == 0 || taken[k]) {
            continue;
        }
        if (s_event_n == OBSERVORE_PEER_EVENTS_MAX) {
            size_t v = event_victim(keep);
            if (v == SIZE_MAX) {
                continue;
            }
            memmove(&s_events[v], &s_events[v + 1],
                    (s_event_n - v - 1) * sizeof(s_events[0]));
            memmove(&keep[v], &keep[v + 1],
                    (s_event_n - v - 1) * sizeof(keep[0]));
            s_event_n--;
        }
        observore_peer_event_t *e = &s_events[s_event_n];
        e->first_us = now_us;
        e->last_us = now_us;
        e->share = *s;
        e->open = true;
        keep[s_event_n] = true;
        s_event_n++;
        if (started && written < max_started) {
            started[written++] = (uint8_t)k;
        }
    }
    PEER_UNLOCK();
    return written;
}

size_t observore_peer_events_copy(observore_peer_event_t *out, size_t max)
{
    size_t n = 0;
    if (!out || !PEER_READY()) {
        return 0;
    }
    PEER_LOCK();
    for (size_t i = s_event_n; i > 0 && n < max; i--) {
        out[n++] = s_events[i - 1];
    }
    PEER_UNLOCK();
    return n;
}

size_t observore_peer_share_text(const observore_peer_share_t *s, char *buf,
                                 size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    if (!s) {
        buf[0] = '\0';
        return 0;
    }
    const char *cls = observore_class_name((observore_class_t)s->cls);
    int n;
    if (s->points == 0) {
        n = snprintf(buf, len, "%s +0 from node %04X (%s): %s", cls,
                     (unsigned)s->node, s->trusted ? "trusted" : "stranger",
                     observore_peer_why_name((observore_peer_why_t)s->why));
    } else if (s->trusted) {
        n = snprintf(buf, len, "%s +%u from node %04X (trusted%s)", cls,
                     (unsigned)s->points, (unsigned)s->node,
                     s->seen_here ? "" : ", not seen here");
    } else if (s->voices > 1) {
        n = snprintf(buf, len,
                     "%s +%u from node %04X (stranger, one voice for %u nodes)",
                     cls, (unsigned)s->points, (unsigned)s->node,
                     (unsigned)s->voices);
    } else {
        n = snprintf(buf, len, "%s +%u from node %04X (stranger)", cls,
                     (unsigned)s->points, (unsigned)s->node);
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

/* ---- where a warning came from -------------------------------------------
 *
 * M_PI is not defined under strict C11, which is why the ultrasound module
 * spells it out too. The mean radius is good to about 0.3 percent against the
 * real, slightly flattened Earth, far under the 10 metre floor the distance
 * bands never go below. */
#define PEER_PI        3.14159265358979323846
#define PEER_EARTH_R_M 6371000.0

/* Distance and initial bearing from point 1 to point 2, haversine in double.
 *
 * Two valid longitudes can differ by 3.6e9, which does not fit an int32_t, so
 * the difference is taken in 64 bits and wrapped to the short way round: 179.9
 * degrees east and 179.9 degrees west are 0.2 degrees apart, not 359.8. The
 * haversine term is clamped to 1 because rounding can push it a hair over for
 * points on opposite sides of the Earth, and sqrt of a negative would be NaN.
 * The caller has already refused an unusable position at either end. */
static void great_circle(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2,
                         double *dist_m, double *bearing_deg)
{
    int64_t dlo = (int64_t)lo2 - lo1;
    if (dlo > 1800000000LL) {
        dlo -= 3600000000LL;
    }
    if (dlo < -1800000000LL) {
        dlo += 3600000000LL;
    }
    const double k = 1e-7 * PEER_PI / 180.0;
    double p1 = la1 * k;
    double p2 = la2 * k;
    double dphi = (double)((int64_t)la2 - la1) * k;
    double dl = (double)dlo * k;
    double s1 = sin(dphi / 2.0);
    double s2 = sin(dl / 2.0);
    double a = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
    if (a > 1.0) {
        a = 1.0;
    }
    *dist_m = 2.0 * PEER_EARTH_R_M * atan2(sqrt(a), sqrt(1.0 - a));

    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / PEER_PI;
    *bearing_deg = (b < 0.0) ? b + 360.0 : b;
}

void observore_peer_locate(bool have_own, int32_t own_lat_e7, int32_t own_lon_e7,
                           bool have_peer, int32_t peer_lat_e7,
                           int32_t peer_lon_e7, observore_peer_reading_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!have_peer || !observore_peer_position_usable(peer_lat_e7, peer_lon_e7)) {
        return;
    }
    if (!have_own || !observore_peer_position_usable(own_lat_e7, own_lon_e7)) {
        out->kind = OBSERVORE_PEER_READ_KNOWN;
        return;
    }

    double d = 0.0;
    double b = 0.0;
    great_circle(own_lat_e7, own_lon_e7, peer_lat_e7, peer_lon_e7, &d, &b);

    /* Whole metres for the bands, saturating (the comparison form also sends a
     * NaN to the saturated value). The two thresholds below are decided on the raw distance, so 19.9
     * metres is nearby and 20.0 is not, whatever the rounding says. */
    double r = d + 0.5;
    out->dist_m = (r < 4294967295.0) ? (uint32_t)r : UINT32_MAX;
    if (d < (double)OBSERVORE_PEER_NEARBY_M) {
        out->kind = OBSERVORE_PEER_READ_NEARBY;
        return;
    }
    out->kind = OBSERVORE_PEER_READ_BEARING;
    out->sector = (uint8_t)((unsigned)((b + 22.5) / 45.0) % 8u);
    out->far = d > (double)OBSERVORE_PEER_FAR_M;
}

const char *observore_peer_sector_name(uint8_t sector)
{
    static const char *const names[8] = {"N", "NE", "E", "SE",
                                         "S", "SW", "W", "NW"};
    return sector < 8 ? names[sector] : "?";
}

/* A distance in the band its precision allows: the nearest 10 metres under a
 * kilometre, a decimal of a kilometre up to 10 km, whole kilometres beyond.
 * Rounding comes first and the band is picked from the rounded value, which
 * is what makes 995 m read "~1.0 km" and 9950 m read "~10 km"; picking the
 * band from the raw value would print "~1000 m" and "~10.0 km". 64-bit
 * arithmetic so nothing overflows near UINT32_MAX. */
static size_t distance_text(uint32_t m, char *buf, size_t len)
{
    uint64_t v = m;
    uint64_t tens = ((v + 5) / 10) * 10;
    int n;
    if (tens < 1000) {
        n = snprintf(buf, len, "~%u m", (unsigned)tens);
    } else {
        uint64_t tenths = (v + 50) / 100;
        if (tenths < 100) {
            n = snprintf(buf, len, "~%u.%u km", (unsigned)(tenths / 10),
                         (unsigned)(tenths % 10));
        } else {
            n = snprintf(buf, len, "~%u km", (unsigned)((v + 500) / 1000));
        }
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

/* What a folded stranger voice says in place of a position: how many senders
 * it stands for and how many of them gave one. */
static size_t fold_text(unsigned voices, unsigned with_pos, char *buf, size_t len)
{
    int n = snprintf(buf, len, "%u nodes, %u with position", voices, with_pos);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

size_t observore_peer_reading_text(const observore_peer_reading_t *r, bool trusted,
                                   bool screen, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    /* Nothing for a stranger on a screen: it is read at a glance and acted
     * on, and an unsigned position is only ever the sender's claim. */
    if (!r || (!trusted && screen)) {
        return 0;
    }
    const char *claims = trusted ? "" : "claims ";
    int n;
    switch (r->kind) {
    case OBSERVORE_PEER_READ_KNOWN:
        n = snprintf(buf, len, "%s", trusted ? "position known" : "position claimed");
        break;
    case OBSERVORE_PEER_READ_NEARBY:
        n = snprintf(buf, len, "%snearby", claims);
        break;
    case OBSERVORE_PEER_READ_BEARING: {
        char dist[24];
        distance_text(r->dist_m, dist, sizeof(dist));
        n = snprintf(buf, len, "%s%s %s%s", claims, dist,
                     observore_peer_sector_name(r->sector),
                     r->far ? (screen ? " (far)" : " (far for BLE)") : "");
        break;
    }
    default:
        return 0;
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < len ? (size_t)n : len - 1;
}

size_t observore_peer_share_where(const observore_peer_share_t *s, bool have_own,
                                  int32_t own_lat_e7, int32_t own_lon_e7,
                                  bool screen, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!s) {
        return 0;
    }
    if (!s->trusted && s->voices > 1) {
        /* A folded row stands for several senders, so it never shows one
         * sender's position. When none of them gave a position there is
         * nothing to say about where, the same as a lone sender that gave
         * none; the voice count is already in the share's own text. */
        if (screen || s->pos_n == 0) {
            return 0;
        }
        return fold_text(s->voices, s->pos_n, buf, len);
    }
    observore_peer_reading_t r;
    observore_peer_locate(have_own, own_lat_e7, own_lon_e7, s->have_pos,
                          s->lat_e7, s->lon_e7, &r);
    return observore_peer_reading_text(&r, s->trusted, screen, buf, len);
}

size_t observore_peer_screen_reserve(const bool *reading, size_t n, bool narrow)
{
    if (n > OBSERVORE_PEER_MAX) {
        n = OBSERVORE_PEER_MAX;
    }
    if (n == 0) {
        return 0;
    }
    size_t rows = n;
    if (narrow && reading && reading[0]) {
        rows++;
    }
    return rows < OBSERVORE_PEER_SCREEN_ROWS ? rows : OBSERVORE_PEER_SCREEN_ROWS;
}

size_t observore_peer_screen_slots(const bool *reading, size_t n, bool narrow,
                                   size_t rows_free, uint8_t *slot)
{
    if (!slot) {
        return 0;
    }
    if (n > OBSERVORE_PEER_MAX) {
        n = OBSERVORE_PEER_MAX;
    }
    if (n == 0) {
        return 0;
    }
    const bool line = narrow && reading && reading[0];
    size_t out = 0;
    for (size_t k = 0; k < n && out < rows_free; k++) {
        slot[out++] = (uint8_t)k;
        if (k == 0 && line && out < rows_free) {
            slot[out++] = OBSERVORE_PEER_SLOT_READING;
        }
    }
    return out;
}
