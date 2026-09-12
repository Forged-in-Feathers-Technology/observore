#include <stdio.h>

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
