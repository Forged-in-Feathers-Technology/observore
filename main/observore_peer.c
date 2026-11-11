#include "observore_peer.h"

#include <string.h>

#include "observore_detect.h"
#include "observore_types.h"

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

int observore_peer_trusted(int64_t now_us)
{
    int n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (!expired(&s_tab[i], now_us) && s_tab[i].trusted) {
            n++;
        }
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

/* Room left under the alert line for evidence that must not escalate by
 * itself: a lone trusted voice, or any stranger. Zero once the number is
 * already at the top of caution or above. */
static unsigned alert_room(uint32_t running)
{
    return running < (uint32_t)(OBSERVORE_SCORE_ALERT - 1)
               ? (unsigned)(OBSERVORE_SCORE_ALERT - 1) - (unsigned)running
               : 0;
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
     * room below alert. They attach only to classes counted here, so this
     * can move the number inside caution and cannot change the level. */
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
                                   alert_room(running)));
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
    observore_peer_weigh(s_tab, s_count, counting_mask, monitors_off_mask,
                         base_score, now_us, out);
}
