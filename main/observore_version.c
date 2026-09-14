#include "observore_version.h"

#include <ctype.h>
#include <string.h>

static const char *skip_v(const char *s)
{
    while (*s == ' ') {
        s++;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    return s;
}

static bool read_number(const char **p, unsigned *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    unsigned n = 0;
    while (isdigit((unsigned char)*s)) {
        /* Clamp rather than wrap. A version field long enough to overflow is
         * not a version, and silently becoming a small number could make a
         * downgrade look like an upgrade. */
        if (n < 100000) {
            n = n * 10 + (unsigned)(*s - '0');
        }
        s++;
    }
    *out = n;
    *p = s;
    return true;
}

bool observore_version_parse(const char *text, observore_version_t *out)
{
    if (!text || !out) {
        return false;
    }
    observore_version_t v = {0};
    const char *p = skip_v(text);

    if (!read_number(&p, &v.major) || *p++ != '.') {
        return false;
    }
    if (!read_number(&p, &v.minor) || *p++ != '.') {
        return false;
    }
    if (!read_number(&p, &v.patch)) {
        return false;
    }
    /* Anything trailing marks a build that is not the tag itself. ESP-IDF
     * writes "-3-gce8e56e"; a "+dirty" suffix means the same thing here. */
    v.dev = (*p != '\0');
    *out = v;
    return true;
}

int observore_version_cmp(const observore_version_t *a,
                          const observore_version_t *b)
{
    if (!a || !b) {
        return 0;
    }
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    if (a->dev != b->dev) {
        return a->dev ? 1 : -1;   /* a build after the tag is ahead of it */
    }
    return 0;
}

bool observore_version_is_newer(const char *candidate, const char *running)
{
    observore_version_t c, r;
    if (!observore_version_parse(candidate, &c) ||
        !observore_version_parse(running, &r)) {
        return false;
    }
    return observore_version_cmp(&c, &r) > 0;
}

bool observore_json_string_field(const char *json, const char *key,
                                 char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    char needle[32];
    size_t klen = strlen(key);
    if (klen + 3 > sizeof(needle)) {
        return false;
    }
    needle[0] = '"';
    memcpy(needle + 1, key, klen);
    needle[klen + 1] = '"';
    needle[klen + 2] = '\0';

    const char *p = strstr(json, needle);
    if (!p) {
        return false;
    }
    p += klen + 2;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p++ != ':') {
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p++ != '"') {
        return false;       /* only string values; a number is not a version */
    }

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_len) {
        /* The document is ours and carries no escapes, but a backslash still
         * must not swallow the closing quote and run off the end. */
        if (*p == '\\') {
            return false;
        }
        out[n++] = *p++;
    }
    if (*p != '"') {
        out[0] = '\0';
        return false;       /* ran out of buffer or out of document */
    }
    out[n] = '\0';
    return n > 0;
}
