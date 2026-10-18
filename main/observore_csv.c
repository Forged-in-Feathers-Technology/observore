#include "observore_csv.h"

size_t observore_csv_field(char *out, size_t cap, const char *in)
{
    if (!out || cap == 0) {
        return 0;
    }
    /* Needs room for two quotes and a terminator before anything useful
     * fits. Below that, produce the shortest valid thing rather than a
     * field with one quote in it, which would corrupt every field after. */
    if (cap < 3) {
        out[0] = '\0';
        return 0;
    }

    size_t n = 0;
    out[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)in; in && *p; p++) {
        /* Anything below space, and DEL. An SSID is thirty-two arbitrary
         * bytes; a tab or an embedded newline is legal in one and a
         * nuisance in every CSV reader. */
        if (*p < 0x20 || *p == 0x7F) {
            continue;
        }
        if (*p == '"') {
            /* Two characters, and both must fit or neither goes: half an
             * escaped quote is worse than a truncated field. */
            if (n + 2 > cap - 2) {
                break;
            }
            out[n++] = '"';
            out[n++] = '"';
            continue;
        }
        if (n + 1 > cap - 2) {
            break;
        }
        out[n++] = (char)*p;
    }
    out[n++] = '"';
    out[n] = '\0';
    return n;
}
