#include "observore_census_wire.h"

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

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
