#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "observore_notify_fmt.h"

/* ------------------------------------------------------------------ */
/* Urgency, mapped per provider                                       */
/* ------------------------------------------------------------------ */

/* Each service uses its own scale, so the detector keeps an abstract one and
 * translates here.  The class table previously stored a Gotify number
 * directly, which quietly made one provider's numbering part of the threat
 * model. */

/* Gotify: 0-10.  8 raises a high-priority alert on Android, 5 is an ordinary
 * notification, 2 is quiet. */
static const int GOTIFY_PRIORITY[] = {2, 5, 7, 8};

/* ntfy: 1-5, named min/low/default/high/urgent. */
static const char *const NTFY_PRIORITY[] = {"low", "default", "high", "urgent"};

/* Pushover: -2..2.  Urgent maps to 1 (high) rather than 2 (emergency)
 * deliberately -- emergency requires retry and expire parameters and keeps
 * re-alerting until a human acknowledges it, which is not a reasonable default
 * for a device that can see a police body camera drive past. */
static const int PUSHOVER_PRIORITY[] = {-1, 0, 1, 1};

static size_t urgency_index(observore_urgency_t u)
{
    return (u <= OBSERVORE_URGENCY_URGENT) ? (size_t)u
                                           : (size_t)OBSERVORE_URGENCY_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Provider metadata                                                  */
/* ------------------------------------------------------------------ */

static const struct {
    const char *name;
    const char *default_url;
    const char *url_hint;
    bool        needs_user;
} PROVIDERS[OBSERVORE_PROVIDER_MAX] = {
    [OBSERVORE_PROVIDER_GOTIFY] = {
        "gotify", "", "https://gotify.example.com", false},
    [OBSERVORE_PROVIDER_NTFY] = {
        "ntfy", "https://ntfy.sh", "https://ntfy.sh/your-topic", false},
    [OBSERVORE_PROVIDER_PUSHOVER] = {
        "pushover", "https://api.pushover.net/1/messages.json",
        "leave blank for api.pushover.net", true},
};

const char *observore_provider_name(observore_provider_t p)
{
    return (p < OBSERVORE_PROVIDER_MAX) ? PROVIDERS[p].name : "?";
}

bool observore_provider_from_name(const char *name, observore_provider_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (int i = 0; i < OBSERVORE_PROVIDER_MAX; i++) {
        if (strcmp(name, PROVIDERS[i].name) == 0) {
            *out = (observore_provider_t)i;
            return true;
        }
    }
    return false;
}

bool observore_provider_needs_user(observore_provider_t p)
{
    return (p < OBSERVORE_PROVIDER_MAX) && PROVIDERS[p].needs_user;
}

const char *observore_provider_default_url(observore_provider_t p)
{
    return (p < OBSERVORE_PROVIDER_MAX) ? PROVIDERS[p].default_url : "";
}

const char *observore_provider_url_hint(observore_provider_t p)
{
    return (p < OBSERVORE_PROVIDER_MAX) ? PROVIDERS[p].url_hint : "";
}

/* ------------------------------------------------------------------ */
/* Encoding helpers                                                   */
/* ------------------------------------------------------------------ */

static void add_header(observore_notify_request_t *r, const char *name,
                       const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void add_header(observore_notify_request_t *r, const char *name,
                       const char *fmt, ...)
{
    if (r->header_count >= OBSERVORE_NOTIFY_MAX_HEADERS) {
        return;
    }
    observore_notify_header_t *h = &r->headers[r->header_count++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->value, sizeof(h->value), fmt, ap);
    va_end(ap);
}

/* Append JSON-escaped text.  Returns the number of bytes written. */
static size_t json_escape_into(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 3 < cap; in++) {
        unsigned char c = (unsigned char)*in;
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
    return o;
}

/* Append form-urlencoded text.  Pushover takes a form body, and an unescaped
 * '&' in an advertised device name would otherwise inject a form field. */
static size_t form_escape_into(char *out, size_t cap, const char *in)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0x0F];
        }
    }
    out[o] = '\0';
    return o;
}

/* Strip a trailing slash so "<url>/" and "<url>" behave the same. */
static void copy_base(char *out, size_t cap, const char *url)
{
    snprintf(out, cap, "%s", url ? url : "");
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') {
        out[--n] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Building                                                           */
/* ------------------------------------------------------------------ */

bool observore_notify_build(observore_provider_t provider,
                            const char *base_url, const char *token,
                            const char *user,
                            const char *title, const char *message,
                            observore_urgency_t urgency,
                            observore_notify_request_t *out)
{
    if (!out || provider >= OBSERVORE_PROVIDER_MAX) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const size_t u = urgency_index(urgency);
    /* Smaller than out->url so a provider can append a path without the
     * result being truncated. */
    char base[sizeof(out->url) - 16];
    copy_base(base, sizeof(base), base_url);
    if (base[0] == '\0') {
        copy_base(base, sizeof(base), observore_provider_default_url(provider));
    }
    if (base[0] == '\0') {
        return false;
    }
    if (observore_provider_needs_user(provider) && (!user || !*user)) {
        return false;
    }

    switch (provider) {
        case OBSERVORE_PROVIDER_GOTIFY: {
            snprintf(out->url, sizeof(out->url), "%s/message", base);
            out->content_type = "application/json";
            if (token && *token) {
                add_header(out, "X-Gotify-Key", "%s", token);
            }
            size_t n = 0;
            n += snprintf(out->body + n, sizeof(out->body) - n, "{\"title\":\"");
            n += json_escape_into(out->body + n, sizeof(out->body) - n, title);
            n += snprintf(out->body + n, sizeof(out->body) - n, "\",\"message\":\"");
            n += json_escape_into(out->body + n, sizeof(out->body) - n, message);
            snprintf(out->body + n, sizeof(out->body) - n,
                     "\",\"priority\":%d}", GOTIFY_PRIORITY[u]);
            return true;
        }

        case OBSERVORE_PROVIDER_NTFY: {
            /* The configured URL already names the topic, e.g.
             * https://ntfy.sh/my-topic, so it is posted to directly. */
            snprintf(out->url, sizeof(out->url), "%s", base);
            out->content_type = "text/plain";
            /* ntfy carries the title and priority as headers and the body is
             * the message itself. */
            add_header(out, "Title", "%s", title ? title : "");
            add_header(out, "Priority", "%s", NTFY_PRIORITY[u]);
            if (token && *token) {
                add_header(out, "Authorization", "Bearer %s", token);
            }
            snprintf(out->body, sizeof(out->body), "%s", message ? message : "");
            return true;
        }

        case OBSERVORE_PROVIDER_PUSHOVER: {
            snprintf(out->url, sizeof(out->url), "%s", base);
            out->content_type = "application/x-www-form-urlencoded";
            size_t n = 0;
            n += snprintf(out->body + n, sizeof(out->body) - n, "token=");
            n += form_escape_into(out->body + n, sizeof(out->body) - n, token);
            n += snprintf(out->body + n, sizeof(out->body) - n, "&user=");
            n += form_escape_into(out->body + n, sizeof(out->body) - n, user);
            n += snprintf(out->body + n, sizeof(out->body) - n, "&title=");
            n += form_escape_into(out->body + n, sizeof(out->body) - n, title);
            n += snprintf(out->body + n, sizeof(out->body) - n, "&message=");
            n += form_escape_into(out->body + n, sizeof(out->body) - n, message);
            snprintf(out->body + n, sizeof(out->body) - n, "&priority=%d",
                     PUSHOVER_PRIORITY[u]);
            return true;
        }

        default:
            return false;
    }
}
