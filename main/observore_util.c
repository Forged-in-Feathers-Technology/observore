#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "observore_util.h"

void observore_json_escape(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t o = 0;
    /* Two bytes is the widest escape emitted below, so reserving three leaves
     * room for the terminator in every branch. */
    for (size_t i = 0; in[i] && o + 3 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c >= 0x20 && c < 0x7F) {
            out[o++] = (char)c;
        } else {
            out[o++] = ' ';
        }
    }
    out[o] = '\0';
}

const char *observore_mac_str(const uint8_t mac[OBSERVORE_MAC_LEN],
                              char out[OBSERVORE_MAC_STR_LEN])
{
    snprintf(out, OBSERVORE_MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return out;
}

/* ------------------------------------------------------------------ */
/* JSON append cursor                                                 */
/* ------------------------------------------------------------------ */

void observore_jb_init(observore_jbuf_t *jb, char *buf, size_t size,
                       size_t reserve)
{
    jb->buf = buf;
    /* One byte for the terminator, plus whatever the closing sequence needs. */
    jb->cap = (size > reserve + 1) ? size - reserve - 1 : 0;
    jb->len = 0;
    jb->full = (jb->cap == 0);
    if (buf && size) {
        buf[0] = '\0';
    }
}

void observore_jb_printf(observore_jbuf_t *jb, const char *fmt, ...)
{
    if (jb->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= jb->cap - jb->len) {
        /* Would have truncated.  Drop it whole rather than leaving half a
         * token behind, and stop accepting writes. */
        jb->buf[jb->len] = '\0';
        jb->full = true;
        return;
    }
    jb->len += (size_t)n;
}

void observore_jb_escape(observore_jbuf_t *jb, const char *s)
{
    if (jb->full || !s) {
        return;
    }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[3];
        size_t w;
        if (c == '"' || c == '\\') {
            esc[0] = '\\'; esc[1] = (char)c; w = 2;
        } else if (c == '\n') {
            esc[0] = '\\'; esc[1] = 'n';     w = 2;
        } else if (c >= 0x20 && c < 0x7F) {
            esc[0] = (char)c;                w = 1;
        } else {
            esc[0] = ' ';                    w = 1;
        }
        if (jb->len + w >= jb->cap) {
            jb->full = true;
            break;
        }
        memcpy(jb->buf + jb->len, esc, w);
        jb->len += w;
    }
    jb->buf[jb->len] = '\0';
}

void observore_jb_close(observore_jbuf_t *jb, const char *tail)
{
    /* Writes into the reserve, which init() held back for exactly this. */
    size_t n = strlen(tail);
    memcpy(jb->buf + jb->len, tail, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}
