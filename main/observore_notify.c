#include <stdio.h>
#include <string.h>

#include "observore_clock.h"
#include "observore_notify.h"
#include "observore_util.h"
#include "observore_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "observore_nvs.h"

static const char *TAG = "observore.notify";

#include "observore_nvs.h"

/* Sending is done from the main loop, so a slow or unreachable server would
 * otherwise stall detection for the full TCP timeout. */
#define HTTP_TIMEOUT_MS 5000
/* How many queued notices to flush per pump.  More than a couple in one pass
 * would hold the loop for seconds on a slow link. */
#define MAX_PER_PUMP 2

typedef struct {
    char                title[OBSERVORE_NOTIFY_TITLE_LEN];
    char                message[OBSERVORE_NOTIFY_MSG_LEN];
    observore_urgency_t urgency;
} observore_notice_t;

static SemaphoreHandle_t s_lock;
static char     s_url[OBSERVORE_NOTIFY_URL_LEN];
static char     s_token[OBSERVORE_NOTIFY_TOKEN_LEN];
static char     s_user[OBSERVORE_NOTIFY_USER_LEN];
static observore_provider_t s_provider = OBSERVORE_PROVIDER_GOTIFY;
static observore_notice_t s_queue[OBSERVORE_NOTIFY_QUEUE];
static size_t   s_head, s_count;
static uint32_t s_sent, s_failed, s_dropped;
static char     s_last_error[64];

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ------------------------------------------------------------------ */

static void load(void)
{
    char prov[16] = {0};
    observore_nvs_item_t items[] = {
        {.key = "gotify_url", .type = OBSERVORE_NVS_STR,
         .buf = s_url,   .len = sizeof(s_url)},
        {.key = "gotify_tok", .type = OBSERVORE_NVS_STR,
         .buf = s_token, .len = sizeof(s_token)},
        {.key = "notify_user", .type = OBSERVORE_NVS_STR,
         .buf = s_user,  .len = sizeof(s_user)},
        {.key = "notify_prov", .type = OBSERVORE_NVS_STR,
         .buf = prov,    .len = sizeof(prov)},
    };
    observore_nvs_read(items, OBSERVORE_ARRLEN(items));
    if (!items[0].found) {
        s_url[0] = '\0';
    }
    if (!items[1].found) {
        s_token[0] = '\0';
    }
    if (!items[2].found) {
        s_user[0] = '\0';
    }
    /* Absent means a device configured before providers existed, which could
     * only have been Gotify. */
    if (!items[3].found || !observore_provider_from_name(prov, &s_provider)) {
        s_provider = OBSERVORE_PROVIDER_GOTIFY;
    }
    if (s_url[0] || observore_provider_default_url(s_provider)[0]) {
        ESP_LOGI(TAG, "notifying via %s: %s",
                 observore_provider_name(s_provider),
                 s_url[0] ? s_url : observore_provider_default_url(s_provider));
    }
}

static void save(void)
{
    const observore_nvs_item_t items[] = {
        {.key = "gotify_url", .type = OBSERVORE_NVS_STR, .buf = s_url},
        {.key = "gotify_tok", .type = OBSERVORE_NVS_STR, .buf = s_token},
        {.key = "notify_user", .type = OBSERVORE_NVS_STR, .buf = s_user},
        {.key = "notify_prov", .type = OBSERVORE_NVS_STR,
         .buf = (void *)observore_provider_name(s_provider)},
    };
    observore_nvs_write(items, OBSERVORE_ARRLEN(items));
}

void observore_notify_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    LOCK();
    memset(s_url, 0, sizeof(s_url));
    memset(s_token, 0, sizeof(s_token));
    memset(s_user, 0, sizeof(s_user));
    s_provider = OBSERVORE_PROVIDER_GOTIFY;
    s_head = s_count = 0;
    s_sent = s_failed = s_dropped = 0;
    s_last_error[0] = '\0';
    load();
    UNLOCK();
}

bool observore_notify_configured(void)
{
    LOCK();
    /* Pushover needs no URL of its own, so "configured" means the provider has
     * everything it needs, not merely that a URL was typed. */
    bool ok = (s_url[0] != '\0' ||
               observore_provider_default_url(s_provider)[0] != '\0') &&
              (!observore_provider_needs_user(s_provider) || s_user[0] != '\0');
    UNLOCK();
    return ok;
}

observore_provider_t observore_notify_provider(void)
{
    LOCK();
    observore_provider_t p = s_provider;
    UNLOCK();
    return p;
}

bool observore_notify_has_user(void)
{
    LOCK();
    bool set = s_user[0] != '\0';
    UNLOCK();
    return set;
}

bool observore_notify_url(char *out, size_t len)
{
    if (!out || len == 0) {
        return false;
    }
    LOCK();
    snprintf(out, len, "%s", s_url);
    bool ok = s_url[0] != '\0';
    UNLOCK();
    return ok;
}

esp_err_t observore_notify_set(observore_provider_t provider, const char *url,
                               const char *token, const char *user)
{
    if (provider >= OBSERVORE_PROVIDER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A URL is required unless the provider supplies its own. */
    bool have_url = url && *url;
    if (!have_url && observore_provider_default_url(provider)[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (have_url && strncmp(url, "http://", 7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (observore_provider_needs_user(provider) && (!user || !*user)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((have_url && strlen(url) >= OBSERVORE_NOTIFY_URL_LEN) ||
        (token && strlen(token) >= OBSERVORE_NOTIFY_TOKEN_LEN) ||
        (user && strlen(user) >= OBSERVORE_NOTIFY_USER_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }
    LOCK();
    s_provider = provider;
    snprintf(s_url, sizeof(s_url), "%s", have_url ? url : "");
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    snprintf(s_user, sizeof(s_user), "%s", user ? user : "");
    save();
    UNLOCK();
    return ESP_OK;
}

esp_err_t observore_notify_clear(void)
{
    LOCK();
    memset(s_url, 0, sizeof(s_url));
    memset(s_token, 0, sizeof(s_token));
    memset(s_user, 0, sizeof(s_user));
    s_provider = OBSERVORE_PROVIDER_GOTIFY;
    s_head = s_count = 0;
    save();
    UNLOCK();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Queue                                                              */
/* ------------------------------------------------------------------ */

static void enqueue(const char *title, const char *message, observore_urgency_t urgency)
{
    LOCK();
    if (!s_url[0]) {
        UNLOCK();
        return;     /* nowhere to send it; do not accumulate */
    }
    if (s_count == OBSERVORE_NOTIFY_QUEUE) {
        /* Drop the oldest.  A detector that stops noticing new things because
         * its outbox is full is worse than one that loses the oldest notice. */
        s_head = (s_head + 1) % OBSERVORE_NOTIFY_QUEUE;
        s_count--;
        s_dropped++;
    }
    observore_notice_t *n = &s_queue[(s_head + s_count) % OBSERVORE_NOTIFY_QUEUE];
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->message, sizeof(n->message), "%s", message);
    n->urgency = urgency;
    s_count++;
    UNLOCK();
}

size_t observore_notify_pending(void)
{
    LOCK();
    size_t n = s_count;
    UNLOCK();
    return n;
}

uint32_t observore_notify_sent(void)    { return s_sent; }
uint32_t observore_notify_failed(void)  { return s_failed; }
uint32_t observore_notify_dropped(void) { return s_dropped; }

const char *observore_notify_last_error(void) { return s_last_error; }

/* ------------------------------------------------------------------ */
/* Triggers                                                           */
/* ------------------------------------------------------------------ */

void observore_notify_event(const observore_event_t *ev)
{
    if (!ev) {
        return;
    }
    char title[OBSERVORE_NOTIFY_TITLE_LEN];
    char msg[OBSERVORE_NOTIFY_MSG_LEN];

    snprintf(title, sizeof(title), "%s detected", observore_class_name(ev->cls));
    char macbuf[OBSERVORE_MAC_STR_LEN];
    /* When it was seen, not when it was sent.
     *
     * Notices are queued while patrolling and flushed on the next uplink
     * window, so delivery can trail detection by twenty minutes -- and the
     * queue exists precisely for the case where that gap is longest. A push
     * that arrives at 03:20 saying a body camera was detected, with no
     * indication of when, is misleading in exactly the situation it matters
     * most. Omitted rather than guessed when the clock has never been set. */
    char seen[24];
    bool dated = observore_clock_iso(ev->last_seen_us, seen, sizeof(seen));

    snprintf(msg, sizeof(msg), "%s%s%s\n%s %s\n%d dBm, via %s, %s%s%s",
             ev->label,
             ev->detail[0] ? " / " : "", ev->detail,
             observore_mac_str(ev->mac, macbuf),
             ev->vendor ? ev->vendor : (ev->addr_random ? "(random)" : ""),
             ev->rssi, observore_source_name(ev->src),
             observore_evidence_name(ev->evidence),
             dated ? "\nseen " : "", dated ? seen : "");

    enqueue(title, msg, observore_class_desc(ev->cls)->notify_urgency);
}

void observore_notify_level(observore_level_t from, observore_level_t to, uint16_t score)
{
    if (to <= from) {
        return;   /* only escalation is news */
    }
    char title[OBSERVORE_NOTIFY_TITLE_LEN];
    char msg[OBSERVORE_NOTIFY_MSG_LEN];
    snprintf(title, sizeof(title), "Observore: %s", observore_level_name(to));
    snprintf(msg, sizeof(msg), "Threat level %s -> %s, score %u.",
             observore_level_name(from), observore_level_name(to), score);
    enqueue(title, msg, to == OBSERVORE_LEVEL_ALERT ? OBSERVORE_URGENCY_URGENT
                                                    : OBSERVORE_URGENCY_NORMAL);
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t send_now(const observore_notice_t *n)
{
    observore_notify_request_t req;

    LOCK();
    bool ok = observore_notify_build(s_provider, s_url, s_token, s_user,
                                     n->title, n->message, n->urgency, &req);
    UNLOCK();
    if (!ok) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "incomplete configuration for this provider");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t cfg = {
        .url = req.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(c, "Content-Type", req.content_type);
    for (size_t i = 0; i < req.header_count; i++) {
        esp_http_client_set_header(c, req.headers[i].name,
                                   req.headers[i].value);
    }
    esp_http_client_set_post_field(c, req.body, strlen(req.body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    /* Judge by the status code when there is one.  Some servers and reverse
     * proxies answer without a Content-Length and simply close, which the
     * client reports as an incomplete read even though the POST was delivered
     * and acknowledged. */
    if (err != ESP_OK && !(status >= 200 && status < 300)) {
        snprintf(s_last_error, sizeof(s_last_error), "%s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        /* 401 and 403 almost always mean a wrong token -- or, for Pushover, a
         * wrong user key -- which is worth saying rather than leaving a bare
         * number. */
        snprintf(s_last_error, sizeof(s_last_error), "HTTP %d%s", status,
                 (status == 401 || status == 403) ? " (bad token or key)" : "");
        return ESP_FAIL;
    }
    s_last_error[0] = '\0';
    return ESP_OK;
}

void observore_notify_pump(void)
{
    if (!observore_notify_configured() ||
        observore_wifi_mode() != OBSERVORE_MODE_UPLINK) {
        return;
    }

    for (int i = 0; i < MAX_PER_PUMP; i++) {
        observore_notice_t notice;
        LOCK();
        if (s_count == 0) {
            UNLOCK();
            return;
        }
        notice = s_queue[s_head];
        UNLOCK();

        if (send_now(&notice) != ESP_OK) {
            s_failed++;
            ESP_LOGW(TAG, "push failed: %s", s_last_error);
            /* Leave it queued: the next pump retries rather than losing it. */
            return;
        }
        LOCK();
        if (s_count > 0) {
            s_head = (s_head + 1) % OBSERVORE_NOTIFY_QUEUE;
            s_count--;
        }
        UNLOCK();
        s_sent++;
    }
}

esp_err_t observore_notify_test(void)
{
    if (!observore_notify_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    observore_notice_t n = {.urgency = OBSERVORE_URGENCY_NORMAL};
    snprintf(n.title, sizeof(n.title), "Observore test");
    snprintf(n.message, sizeof(n.message),
             "Notifications are working. Sent from the console.");
    esp_err_t err = send_now(&n);
    if (err == ESP_OK) {
        s_sent++;
    } else {
        s_failed++;
    }
    return err;
}
