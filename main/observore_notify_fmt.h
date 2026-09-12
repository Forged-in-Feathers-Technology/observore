#pragma once

#include <stdbool.h>
#include <stddef.h>

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

const char *observore_provider_name(observore_provider_t provider);
bool observore_provider_from_name(const char *name, observore_provider_t *out);

/* Whether the provider needs a second credential beyond the token, and the
 * URL to offer when none is configured. */
bool observore_provider_needs_user(observore_provider_t provider);
const char *observore_provider_default_url(observore_provider_t provider);
/* What the provider calls the thing the URL points at, for the console. */
const char *observore_provider_url_hint(observore_provider_t provider);
