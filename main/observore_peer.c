#include "observore_peer.h"

#include <string.h>

#include "observore_detect.h"
#include "observore_types.h"

#define FLAG_POSITION 0x01
#define FLAG_TAG      0x02

#define BASE_LEN      15    /* magic through age, with nothing optional */
#define POSITION_LEN   8
#define TAG_LEN        4

static observore_peer_warning_t s_tab[OBSERVORE_PEER_MAX];
static size_t s_count;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The same rule the drone decoder applies, for the same reason: zero/zero is
 * a real place in the Gulf of Guinea, and a node that has no fix sends
 * zeroes. Repeating it as a position would be a confident lie. */
static bool position_usable(int32_t lat_e7, int32_t lon_e7)
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
    if (flags & FLAG_TAG)      { need += TAG_LEN; }
    if (len < need) {
        return false;      /* it claims more than arrived */
    }

    memset(out, 0, sizeof(*out));
    out->node  = le32(&payload[6]);
    out->cls   = payload[10];
    out->seq   = le16(&payload[11]);
    out->age_s = le16(&payload[13]);

    /* A class this build does not have is a node that knows about something
     * we do not. Kept as unknown rather than dropped: that a neighbour is
     * warning at all is the useful part, and inventing a class number we
     * cannot name would be worse than admitting we cannot name it. */
    if (out->cls >= OBSERVORE_CLASS_MAX) {
        out->cls = OBSERVORE_CLASS_UNKNOWN;
    }

    if (flags & FLAG_POSITION) {
        int32_t lat = (int32_t)le32(&payload[15]);
        int32_t lon = (int32_t)le32(&payload[19]);
        if (position_usable(lat, lon)) {
            out->lat_e7 = lat;
            out->lon_e7 = lon;
            out->have_pos = true;
        }
    }

    /* A tag is parsed for its length and then ignored. There is no key
     * management yet, so nothing can be verified, and a tag that cannot be
     * checked must not be mistaken for one that has been: `trusted` stays
     * false. Claiming otherwise is how an unsigned warning would end up
     * carrying the weight of a signed one. */
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

void observore_peer_init(void)
{
    s_count = 0;
    memset(s_tab, 0, sizeof(s_tab));
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

bool observore_peer_note(const observore_peer_warning_t *w, int64_t now_us)
{
    if (!w) {
        return false;
    }

    /* One entry per node: the latest thing a neighbour said, not a log of
     * everything it has ever said. A node repeating itself must not be able
     * to fill the table and push other nodes out, which is the cheapest
     * attack there is on a structure like this. */
    for (size_t i = 0; i < s_count; i++) {
        if (s_tab[i].node != w->node) {
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

    /* Full: the oldest goes. Preferring an expired entry first, so a stale
     * warning never costs a live one its place. */
    size_t oldest = 0;
    for (size_t i = 1; i < s_count; i++) {
        if (s_tab[i].heard_us < s_tab[oldest].heard_us) {
            oldest = i;
        }
    }
    s_tab[oldest] = *w;
    s_tab[oldest].heard_us = now_us;
    return true;
}

size_t observore_peer_recent(observore_peer_warning_t *out, size_t max,
                             int64_t now_us)
{
    if (!out || max == 0) {
        return 0;
    }
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
    return n;
}

void observore_peer_counts(int64_t now_us, int *nodes, int *warnings)
{
    int live = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (!expired(&s_tab[i], now_us)) {
            live++;
        }
    }
    /* One entry per node, so these are the same number -- reported as two
     * because they will stop being the same if a node is ever allowed more
     * than one standing warning, and a caller should not have to find that
     * out by the number changing meaning. */
    if (nodes)    { *nodes = live; }
    if (warnings) { *warnings = live; }
}
