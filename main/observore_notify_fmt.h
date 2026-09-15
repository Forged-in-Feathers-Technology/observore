#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Building a notification request, separated from sending one.
 *
 * Kept free of ESP-IDF so the three wire formats can be tested on the host.
 * Getting a provider's shape subtly wrong is otherwise only discoverable by
 * owning an account with that provider. */

typedef enum {
    OBSERVORE_URGENCY_LOW,      /* street furniture: a camera vendor nearby */
    OBSERVORE_URGENCY_NORMAL,
    OBSERVORE_URGENCY_HIGH,
    OBSERVORE_URGENCY_URGENT,   /* a body camera or a licence-plate reader */
} observore_urgency_t;

typedef enum {
    OBSERVORE_PROVIDER_GOTIFY = 0,
    OBSERVORE_PROVIDER_NTFY,
    OBSERVORE_PROVIDER_PUSHOVER,
    /* A JSON POST to any URL, with an optional bearer token. One format that
     * reaches Home Assistant, n8n, Node-RED, Apprise, signal-cli and anything
     * on the LAN that accepts a POST. */
    OBSERVORE_PROVIDER_WEBHOOK,
    /* The Bot API. The token is the bot's, the second credential is the chat. */
    OBSERVORE_PROVIDER_TELEGRAM,
    OBSERVORE_PROVIDER_MAX,
} observore_provider_t;

#define OBSERVORE_NOTIFY_MAX_HEADERS 3

typedef struct {
    char name[24];
    char value[128];
} observore_notify_header_t;

typedef struct {
    char                      url[224];
    char                      body[768];
    const char               *content_type;
    observore_notify_header_t headers[OBSERVORE_NOTIFY_MAX_HEADERS];
    size_t                    header_count;
} observore_notify_request_t;

/* Build one request.  `base_url` is what the user configured; `user` is only
 * meaningful for providers that need a second credential.  Returns false when
 * the configuration cannot produce a valid request. */
bool observore_notify_build(observore_provider_t provider,
                            const char *base_url, const char *token,
                            const char *user,
                            const char *title, const char *message,
                            observore_urgency_t urgency,
                            observore_notify_request_t *out);

/* One finding, as it will appear in a digest.
 *
 * Six lines is not arbitrary. The whole request has to fit in `body` above, and
 * Pushover is the tightest of the three: it form-encodes, which turns every
 * colon in a MAC into %3A and every newline into %0A, so a 60-character line
 * costs closer to 100 on the wire. Six lines plus a title and the token and
 * user fields lands inside 768 with room to spare; eight does not. */
#define OBSERVORE_DIGEST_MAX_LINES 6
#define OBSERVORE_DIGEST_LINE_LEN  64
#define OBSERVORE_DIGEST_TITLE_LEN 80
#define OBSERVORE_DIGEST_BODY_LEN  448

typedef struct {
    uint8_t     rank;   /* class points; higher is listed first */
    int8_t      rssi;   /* tiebreak within a rank, closest first */
    const char *cls;    /* class name, for the census in the title */
    const char *line;   /* the line itself */
} observore_digest_entry_t;

/* Combine findings into one title and body, ordered by rank.
 *
 * Sorts `entries` in place. Returns how many made it into the body; anything
 * beyond that is accounted for by a trailing "+N more" rather than dropped
 * silently, because a digest that quietly loses its tail is worse than one that
 * admits to it. The title always counts everything.
 *
 * `headline` is optional and leads the title when present, so a level change
 * reads as "alert: 6 findings (1 drone, 5 followers)". */
size_t observore_digest_build(observore_digest_entry_t *entries, size_t count,
                              const char *headline,
                              char *title, size_t title_len,
                              char *body, size_t body_len);

const char *observore_provider_name(observore_provider_t provider);
bool observore_provider_from_name(const char *name, observore_provider_t *out);

/* Whether the provider needs a second credential beyond the token, and the
 * URL to offer when none is configured. */
bool observore_provider_needs_user(observore_provider_t provider);
const char *observore_provider_default_url(observore_provider_t provider);
/* What the provider calls the thing the URL points at, for the console. */
const char *observore_provider_url_hint(observore_provider_t provider);
