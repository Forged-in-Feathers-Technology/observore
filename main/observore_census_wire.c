#include "observore_census_wire.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_OBSERVORE_MESH_CENSUS

/* Little-endian by byte shifts, never a cast through a wider pointer: the
 * bytes are a contract between firmware versions, and an unaligned or
 * big-endian read is not the contract. The same idiom as the advert packer in
 * observore_peer.c. */
static void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t le16_get(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void le32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t le32_get(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

size_t observore_cx_put_hdr(const observore_cx_hdr_t *h, uint8_t *out)
{
    out[0] = OBSERVORE_CX_MAGIC0;
    out[1] = OBSERVORE_CX_MAGIC1;
    out[2] = OBSERVORE_CX_VERSION;
    out[3] = OBSERVORE_CX_KIND_CENSUS;
    le16_put(out + 4, h->node);
    le16_put(out + 6, h->day);
    le16_put(out + 8, h->window);
    out[10] = h->index;
    out[11] = h->total;
    out[12] = h->count;
    memcpy(out + OBSERVORE_CX_NONCE_OFF, h->nonce, OBSERVORE_CX_NONCE_LEN);
    return OBSERVORE_CX_HDR_LEN;
}

size_t observore_cx_put_entry(const observore_census_entry_t *e, uint8_t *out)
{
    le32_put(out, e->id);
    le16_put(out + 4, e->days);
    le16_put(out + 6, e->last_day);
    for (int i = 0; i < OBSERVORE_CENSUS_ADDRS; i++) {
        le16_put(out + 8 + 2 * i, i < e->addr_n ? e->addr[i] : 0);
    }
    out[24] = e->addr_n;
    out[25] = e->addr_over;
    return OBSERVORE_CX_ENTRY_LEN;
}

observore_cx_frame_t observore_cx_check_frame(const uint8_t *frame, size_t len,
                                              observore_cx_hdr_t *hdr)
{
    /* Length before any read: a frame shorter than the header has no version
     * to look at, and a neighbour's gadget is the likeliest reason. */
    if (!frame || len < OBSERVORE_CX_HDR_LEN ||
        frame[0] != OBSERVORE_CX_MAGIC0 || frame[1] != OBSERVORE_CX_MAGIC1) {
        return OBSERVORE_CX_FRAME_FOREIGN;
    }
    if (frame[2] != OBSERVORE_CX_VERSION) {
        return OBSERVORE_CX_FRAME_VERSION;
    }
    const uint8_t index = frame[10], total = frame[11], count = frame[12];
    if (frame[3] != OBSERVORE_CX_KIND_CENSUS ||
        count == 0 || count > OBSERVORE_CX_ENTRIES_MAX ||
        total == 0 || total > OBSERVORE_CX_CHUNKS_MAX ||
        index >= total) {
        return OBSERVORE_CX_FRAME_MALFORMED;
    }
    if (len != (size_t)OBSERVORE_CX_HDR_LEN +
                   (size_t)OBSERVORE_CX_ENTRY_LEN * count +
                   OBSERVORE_CX_TAG_LEN) {
        return OBSERVORE_CX_FRAME_MALFORMED;
    }
    if (hdr) {
        hdr->node   = le16_get(frame + 4);
        hdr->day    = le16_get(frame + 6);
        hdr->window = le16_get(frame + 8);
        hdr->index  = index;
        hdr->total  = total;
        hdr->count  = count;
        memcpy(hdr->nonce, frame + OBSERVORE_CX_NONCE_OFF, OBSERVORE_CX_NONCE_LEN);
    }
    return OBSERVORE_CX_FRAME_OK;
}

bool observore_cx_get_entry(const uint8_t *in, int sender_day,
                            observore_census_entry_t *out)
{
    if (!in || !out || sender_day < 0 || sender_day > OBSERVORE_CENSUS_DAY_MAX) {
        return false;
    }
    observore_census_entry_t e;
    memset(&e, 0, sizeof(e));
    e.id        = le32_get(in);
    e.days      = le16_get(in + 4);
    e.last_day  = le16_get(in + 6);
    uint16_t addr[OBSERVORE_CENSUS_ADDRS];
    for (int i = 0; i < OBSERVORE_CENSUS_ADDRS; i++) {
        addr[i] = le16_get(in + 8 + 2 * i);
    }
    e.addr_n    = in[24];
    e.addr_over = in[25];

    if (e.id == 0 || e.addr_n > OBSERVORE_CENSUS_ADDRS || e.addr_over > 1 ||
        e.days == 0) {
        return false;
    }
    /* A day the sender has not reached yet cannot have been seen, and a last
     * day a whole window back has nothing left in it. */
    if ((int)e.last_day > sender_day ||
        sender_day - (int)e.last_day >= OBSERVORE_CENSUS_WINDOW) {
        return false;
    }
    /* The mask as it reads on the sender's day, in 32-bit arithmetic so the
     * shift cannot overflow before it is masked down to the window. */
    uint32_t aged = ((uint32_t)e.days << (sender_day - (int)e.last_day)) & 0xFFFFu;
    if (aged == 0) {
        return false;
    }
    for (int i = 0; i < e.addr_n; i++) {
        e.addr[i] = addr[i];
    }
    *out = e;
    return true;
}

bool observore_cx_day_ok(int sender_day, int my_day)
{
    /* Out of range comes first: NO_DAY is -1, and -1 is one away from day 0. */
    if (sender_day < 0 || sender_day > OBSERVORE_CENSUS_DAY_MAX ||
        my_day < 0 || my_day > OBSERVORE_CENSUS_DAY_MAX) {
        return false;
    }
    int d = sender_day - my_day;
    return d >= -OBSERVORE_CX_DAY_SKEW && d <= OBSERVORE_CX_DAY_SKEW;
}

uint8_t observore_cx_plan(size_t n)
{
    size_t chunks = (n + OBSERVORE_CX_ENTRIES_MAX - 1) / OBSERVORE_CX_ENTRIES_MAX;
    return (uint8_t)(chunks > OBSERVORE_CX_CHUNKS_MAX ? OBSERVORE_CX_CHUNKS_MAX : chunks);
}

void observore_cx_chunk(size_t n, uint8_t index, size_t *first, uint8_t *count)
{
    /* Whatever the caller holds, a window carries no more than it can number. */
    const size_t cap = (size_t)OBSERVORE_CX_ENTRIES_MAX * OBSERVORE_CX_CHUNKS_MAX;
    if (n > cap) {
        n = cap;
    }
    size_t f = (size_t)index * OBSERVORE_CX_ENTRIES_MAX;
    size_t left = n > f ? n - f : 0;
    if (first) {
        *first = f < n ? f : n;
    }
    if (count) {
        *count = (uint8_t)(left > OBSERVORE_CX_ENTRIES_MAX ? OBSERVORE_CX_ENTRIES_MAX : left);
    }
}

observore_cx_seen_t observore_cx_heard_note(observore_cx_heard_t *h, uint16_t node,
                                            uint16_t window, uint8_t index)
{
    /* A verified frame has an index under its total, which is under eight. One
     * that is not is nothing to hold, and says so by being old news. */
    if (index >= OBSERVORE_CX_CHUNKS_MAX) {
        return OBSERVORE_CX_DUPLICATE;
    }
    for (uint8_t i = 0; i < h->n; i++) {
        if (h->f[i].node != node) {
            continue;
        }
        if (h->f[i].window != window) {
            h->f[i].window = window;
            h->f[i].chunks = 0;
        }
        const uint8_t bit = (uint8_t)(1u << index);
        if (h->f[i].chunks & bit) {
            return OBSERVORE_CX_DUPLICATE;
        }
        h->f[i].chunks |= bit;
        return OBSERVORE_CX_NEW;
    }
    if (h->n >= OBSERVORE_CX_FRIENDS_MAX) {
        if (h->crowded < UINT16_MAX) {
            h->crowded++;
        }
        return OBSERVORE_CX_CROWDED;
    }
    h->f[h->n].node = node;
    h->f[h->n].window = window;
    h->f[h->n].chunks = (uint8_t)(1u << index);
    h->n++;
    return OBSERVORE_CX_NEW;
}

const char *observore_cx_mode_name(uint8_t mode)
{
    switch (mode) {
    case OBSERVORE_CX_OFF:      return "off";
    case OBSERVORE_CX_LISTEN:   return "listen";
    case OBSERVORE_CX_EXCHANGE: return "exchange";
    default:                    return "?";
    }
}

bool observore_cx_mode_parse(const char *s, uint8_t *mode)
{
    if (!s || !mode) {
        return false;
    }
    for (uint8_t m = OBSERVORE_CX_OFF; m <= OBSERVORE_CX_EXCHANGE; m++) {
        if (strcmp(s, observore_cx_mode_name(m)) == 0) {
            *mode = m;
            return true;
        }
    }
    return false;
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

bool observore_cx_at_parse(const char *s, uint16_t *minute_of_day)
{
    if (!s || !minute_of_day || strlen(s) != 5 || s[2] != ':' ||
        !digit(s[0]) || !digit(s[1]) || !digit(s[3]) || !digit(s[4])) {
        return false;
    }
    int hh = (s[0] - '0') * 10 + (s[1] - '0');
    int mm = (s[3] - '0') * 10 + (s[4] - '0');
    if (hh > 23 || mm > 59) {
        return false;
    }
    *minute_of_day = (uint16_t)(hh * 60 + mm);
    return true;
}

bool observore_cx_channel_ok(int ch)
{
    return ch >= 1 && ch <= 13;
}

#define US_PER_S 1000000LL

observore_cx_decision_t observore_cx_decide(const observore_cx_when_t *w)
{
    if (w->mode != OBSERVORE_CX_LISTEN && w->mode != OBSERVORE_CX_EXCHANGE) {
        return OBSERVORE_CX_SKIP_OFF;
    }
    if (!w->have_secret) {
        return OBSERVORE_CX_SKIP_NO_SECRET;
    }
    if (!w->clock_valid || w->today < 0 || w->today > OBSERVORE_CENSUS_DAY_MAX) {
        return OBSERVORE_CX_SKIP_NO_CLOCK;
    }
    if (w->manual) {
        if (w->last_close_us != 0 &&
            w->now_us - w->last_close_us < (int64_t)OBSERVORE_CX_MIN_GAP_S * US_PER_S) {
            return OBSERVORE_CX_SKIP_TOO_SOON;
        }
        return OBSERVORE_CX_OPEN_NOW;
    }
    if (w->last_day == w->today) {
        return OBSERVORE_CX_WAIT;
    }
    const int at = (int)w->at_min * 60;
    if (w->sod >= at && w->sod < at + OBSERVORE_CX_GRACE_S) {
        return OBSERVORE_CX_OPEN_SCHEDULED;
    }
    return OBSERVORE_CX_WAIT;
}

bool observore_cx_should_close(int64_t opened_us, int64_t now_us)
{
    return now_us - opened_us >= (int64_t)OBSERVORE_CX_WINDOW_MAX_S * US_PER_S;
}

unsigned observore_cx_remaining_s(int64_t opened_us, int64_t now_us)
{
    int64_t left = (int64_t)OBSERVORE_CX_WINDOW_MAX_S * US_PER_S - (now_us - opened_us);
    if (left <= 0) {
        return 0;
    }
    if (left > (int64_t)OBSERVORE_CX_WINDOW_MAX_S * US_PER_S) {
        return OBSERVORE_CX_WINDOW_MAX_S;   /* a clock read from before the opening */
    }
    return (unsigned)((left + US_PER_S - 1) / US_PER_S);
}

long observore_cx_seconds_until(const observore_cx_when_t *w)
{
    if ((w->mode != OBSERVORE_CX_LISTEN && w->mode != OBSERVORE_CX_EXCHANGE) ||
        !w->have_secret || !w->clock_valid || w->today < 0 ||
        w->today > OBSERVORE_CENSUS_DAY_MAX) {
        return -1;
    }
    const long day_s = 86400;
    const long at = (long)w->at_min * 60;
    if (w->last_day != w->today) {
        if (w->sod < at) {
            return at - w->sod;
        }
        if (w->sod < at + OBSERVORE_CX_GRACE_S) {
            return 0;
        }
    }
    return day_s - w->sod + at;
}

size_t observore_cx_result_text(const observore_cx_result_t *r, char *buf, size_t len)
{
    if (!r || !buf || len == 0) {
        return 0;
    }
    char *p = buf;
    size_t left = len;
    int n;
    uint8_t friends = r->friends > OBSERVORE_CX_FRIENDS_MAX ? OBSERVORE_CX_FRIENDS_MAX
                                                             : r->friends;
    const bool sends = r->mode == OBSERVORE_CX_EXCHANGE;

/* Append one piece, keeping the text clamped and terminated whatever is left. */
#define PUT(...)                                                              \
    do {                                                                      \
        if (left > 1) {                                                       \
            n = snprintf(p, left, __VA_ARGS__);                               \
            if (n > 0) {                                                      \
                size_t w_ = (size_t)n < left - 1 ? (size_t)n : left - 1;      \
                p += w_;                                                      \
                left -= w_;                                                   \
            }                                                                 \
        }                                                                     \
    } while (0)

    PUT("%s, %u s%s: ", sends ? "share" : observore_cx_mode_name(r->mode),
        (unsigned)r->secs, r->interrupted ? ", interrupted" : "");
    if (sends) {
        PUT("sent %u entries in %u rounds, ", (unsigned)r->sent, (unsigned)r->rounds);
    }
    PUT("heard %u", (unsigned)friends);
    if (friends > 0) {
        PUT(" (");
        for (uint8_t i = 0; i < friends; i++) {
            PUT("%s%04X", i ? " " : "", (unsigned)r->friend_node[i]);
        }
        PUT(")");
    }
    PUT(", merged %u, rejected %u bad tag, %u day skew, %u malformed, %u other ESP-NOW",
        (unsigned)r->merged, (unsigned)r->bad_tag, (unsigned)r->day_skew,
        (unsigned)r->malformed, (unsigned)r->foreign);
#undef PUT
    *p = '\0';
    return (size_t)(p - buf);
}

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
