#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "observore_clock.h"
#include "observore_notify.h"

#include "sdkconfig.h"
#if CONFIG_OBSERVORE_NOTIFIER
#include "observore_util.h"
#include "observore_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "observore_nvs.h"

static const char *TAG = "observore.notify";

#include "observore_nvs.h"

/* Sending is done from the main loop, so a slow or unreachable server would
 * otherwise stall detection for the full TCP timeout. */
#define HTTP_TIMEOUT_MS 5000

/* One queued finding.
 *
 * Deliberately small. Everything queued in an uplink window is combined into a
 * single digest, so a notice carries one list line rather than a whole message,
 * and the queue costs about 2 KB instead of the 6.6 KB it did when each entry
 * held its own title and a 224-character body. On a part where the internal
 * heap has been measured at 184 bytes free, static savings are heap. */
typedef struct {
    char                line[OBSERVORE_DIGEST_LINE_LEN];
    char                cls[16];
    uint8_t             rank;
    int8_t              rssi;
    observore_urgency_t urgency;
} observore_notice_t;

static SemaphoreHandle_t s_lock;
static char     s_url[OBSERVORE_NOTIFY_URL_LEN];
static char     s_token[OBSERVORE_NOTIFY_TOKEN_LEN];
static char     s_user[OBSERVORE_NOTIFY_USER_LEN];
static observore_provider_t s_provider = OBSERVORE_PROVIDER_GOTIFY;
static observore_notice_t s_queue[OBSERVORE_NOTIFY_QUEUE];
static size_t   s_head, s_count;
/* Set when the threat level rises, consumed by the next digest. A level change
 * is context for the findings rather than a finding of its own, so it leads the
 * title instead of occupying a line. */
static char     s_headline[16];
static uint16_t s_headline_score;
/* A version worth mentioning, cleared once it has been. */
static char     s_update_version[24];
static uint32_t s_sent, s_failed, s_dropped;
static char     s_last_error[64];

/* Backoff after a failed delivery.
 *
 * Measured, not imagined: a device left overnight with an unreachable notifier
 * logged 10,019 failed attempts and zero successes in seven hours -- one every
 * 2.6 seconds, for as long as it was associated, forever. The pump had no
 * backoff at all. It ran from the main loop, and a failure simply returned so
 * the next pass could try again immediately.
 *
 * That is expensive in the three ways that matter on this device. Each attempt
 * sets up a TLS connection, which is tens of kilobytes on a part that had
 * about 35 KB free; it burns radio time in the narrow window the device is
 * actually associated; and it drains a battery for nothing. An endpoint that
 * is unreachable now is usually unreachable in two seconds' time -- the
 * common causes are a firewalled segment, a wrong URL, or a server that is
 * down, none of which resolve on that timescale.
 *
 * Doubling from 30 seconds to a 15-minute ceiling keeps a transient outage
 * recovering quickly while making a permanent one cost almost nothing. The
 * notices stay queued throughout; this delays retries, it never discards. */
#define BACKOFF_MIN_US (30 * 1000000LL)
#define BACKOFF_MAX_US (15 * 60 * 1000000LL)

static int64_t  s_retry_after_us;      /* do not attempt before this */
static int64_t  s_backoff_us;          /* current delay, 0 when healthy */
static uint32_t s_consecutive_failures;

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
        /* Host only. A webhook URL is frequently the credential -- Discord puts
         * a token in the path and Home Assistant's webhook id is the whole of
         * its authentication -- and serial logs end up pasted into bug reports.
         * The host says where it goes, which is all a log needs. */
        const char *url = s_url[0] ? s_url : observore_provider_default_url(s_provider);
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const char *end = strchr(host, '/');
        ESP_LOGI(TAG, "notifying via %s: %.*s",
                 observore_provider_name(s_provider),
                 end ? (int)(end - host) : (int)strlen(host), host);
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
    s_consecutive_failures = 0;
    s_backoff_us = 0;
    s_retry_after_us = 0;
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
    if ((have_url && strlen(url) >= OBSERVORE_NOTIFY_URL_LEN) ||
        (token && strlen(token) >= OBSERVORE_NOTIFY_TOKEN_LEN) ||
        (user && strlen(user) >= OBSERVORE_NOTIFY_USER_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }

    LOCK();
    /* A blank credential keeps the stored one -- but only for the same
     * provider.
     *
     * Saving with the token box empty used to wipe the token, so changing a
     * URL, or trying a new provider and coming back, cost people a credential
     * they had not asked to lose. Keeping it is the obvious fix and it is
     * wrong across a provider change: the stored token belongs to the old
     * service, and carrying it over would send a Gotify key as the bearer
     * header of whatever webhook was just typed in. A credential must never
     * travel to a service it was not issued for, so a provider switch with
     * nothing entered starts clean, and the console says so. */
    bool same = (provider == s_provider);
    bool keep_token = same && (!token || !*token) && s_token[0];
    bool keep_user  = same && (!user  || !*user)  && s_user[0];

    if (observore_provider_needs_user(provider) && !keep_user && (!user || !*user)) {
        UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }

    s_provider = provider;
    snprintf(s_url, sizeof(s_url), "%s", have_url ? url : "");
    if (!keep_token) {
        snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    }
    if (!keep_user) {
        snprintf(s_user, sizeof(s_user), "%s", user ? user : "");
    }
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

static void enqueue(const char *cls, uint8_t rank, int8_t rssi,
                    const char *line, observore_urgency_t urgency)
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
    snprintf(n->line, sizeof(n->line), "%s", line);
    snprintf(n->cls, sizeof(n->cls), "%s", cls ? cls : "");
    n->rank    = rank;
    n->rssi    = rssi;
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

/* Seconds until the next attempt, or 0 when not backing off.  Reported so a
 * notifier that has gone quiet can say why it is quiet: "failing, next try in
 * 900s" is a diagnosis, where a rising failure count on its own is a puzzle. */
uint32_t observore_notify_retry_in_s(void)
{
    if (!s_retry_after_us) {
        return 0;
    }
    int64_t left = s_retry_after_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}
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
    char macbuf[OBSERVORE_MAC_STR_LEN];
    char line[OBSERVORE_DIGEST_LINE_LEN];

    /* One line, because it is going into a list beside other findings.
     *
     * What survives the compression is what distinguishes one finding from
     * another: the class, the address, how close it is, and who made it. The
     * timestamp does not -- a digest is sent within one uplink window of the
     * detections in it, so "when" is answered by the notification's own arrival
     * time to a far better resolution than a truncated field would manage. The
     * device table keeps the full record either way. */
    const char *who = ev->detail[0] ? ev->detail
                    : (ev->vendor ? ev->vendor
                                  : (ev->addr_random ? "random" : ""));
    snprintf(line, sizeof(line), "%s %s %d dBm%s%s",
             observore_class_name(ev->cls),
             observore_mac_str(ev->mac, macbuf),
             ev->rssi,
             who && who[0] ? " " : "", who ? who : "");

    const observore_class_desc_t *d = observore_class_desc(ev->cls);
    enqueue(observore_class_name(ev->cls), d->points, ev->rssi, line,
            d->notify_urgency);
}

void observore_notify_level(observore_level_t from, observore_level_t to, uint16_t score)
{
    if (to <= from) {
        return;   /* only escalation is news */
    }
    LOCK();
    if (s_url[0]) {
        snprintf(s_headline, sizeof(s_headline), "%s", observore_level_name(to));
        s_headline_score = score;
    }
    UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t send_now(const char *title, const char *message,
                          observore_urgency_t urgency)
{
    observore_notify_request_t req;

    LOCK();
    bool ok = observore_notify_build(s_provider, s_url, s_token, s_user,
                                     title, message, urgency, &req);
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

void observore_notify_update_available(const char *version)
{
    if (!version || !*version) {
        return;
    }
    LOCK();
    if (s_url[0]) {
        snprintf(s_update_version, sizeof(s_update_version), "%s", version);
    }
    UNLOCK();
}

void observore_notify_pump(void)
{
    if (!observore_notify_configured() ||
        observore_wifi_mode() != OBSERVORE_MODE_UPLINK) {
        return;
    }
    if (s_retry_after_us && esp_timer_get_time() < s_retry_after_us) {
        return;                       /* still backing off */
    }

    /* Everything queued goes in one message.
     *
     * The queue used to be drained a couple of notices at a time, each through
     * its own TLS handshake and certificate bundle verification. Six findings
     * meant six handshakes inside one thirty-second window, and the internal
     * heap was measured at 184 bytes free doing exactly that -- eight bytes
     * above the figure that once cost 10,019 consecutive delivery failures.
     *
     * One handshake carrying a ranked list costs what one handshake costs, and
     * it is also the better report: six separate pushes for one walk past a row
     * of parked cars is noise. */
    /* Static, not automatic. These come to about 2.8 KB, and the main task
     * stack is 6 KB with the TLS request struct already on it further down --
     * the first version of this put them on the stack and the device panicked
     * with a stack protection fault the moment an uplink window opened with
     * four findings queued. Safe as statics because the pump runs only from the
     * main loop, which is also the only caller of observore_notify_event(). */
    static observore_digest_entry_t entries[OBSERVORE_NOTIFY_QUEUE];
    static char lines[OBSERVORE_NOTIFY_QUEUE][OBSERVORE_DIGEST_LINE_LEN];
    static char classes[OBSERVORE_NOTIFY_QUEUE][16];
    static char headline[sizeof(s_headline)];
    static char update_version[sizeof(s_update_version)];
    uint16_t headline_score;
    observore_urgency_t urgency = OBSERVORE_URGENCY_LOW;
    size_t count = 0;

    LOCK();
    snprintf(headline, sizeof(headline), "%s", s_headline);
    snprintf(update_version, sizeof(update_version), "%s", s_update_version);
    headline_score = s_headline_score;
    for (size_t i = 0; i < s_count; i++) {
        const observore_notice_t *n = &s_queue[(s_head + i) % OBSERVORE_NOTIFY_QUEUE];
        snprintf(lines[count], OBSERVORE_DIGEST_LINE_LEN, "%s", n->line);
        snprintf(classes[count], sizeof(classes[count]), "%s", n->cls);
        entries[count].rank = n->rank;
        entries[count].rssi = n->rssi;
        entries[count].cls  = classes[count];
        entries[count].line = lines[count];
        if (n->urgency > urgency) {
            urgency = n->urgency;
        }
        count++;
    }
    size_t queued = s_count;
    UNLOCK();

    if (count == 0 && headline[0] == '\0' && update_version[0] == '\0') {
        return;
    }

    static char title[OBSERVORE_DIGEST_TITLE_LEN];
    static char body[OBSERVORE_DIGEST_BODY_LEN];

    if (count == 0 && headline[0] == '\0') {
        /* Nothing detected, but a release appeared. Worth one message: a device
         * that only mentions updates alongside findings would stay quiet
         * forever in exactly the place it is working best. */
        snprintf(title, sizeof(title), "Observore: %s available", update_version);
        snprintf(body, sizeof(body),
                 "A newer firmware is published. Install it from the console.");
        urgency = OBSERVORE_URGENCY_LOW;
    } else if (count == 0) {
        /* A level rose without any single finding crossing the reporting bar,
         * which happens when a device already known gains enough sightings to
         * move the score. Still worth saying, and it is the whole message. */
        snprintf(title, sizeof(title), "Observore: %s", headline);
        snprintf(body, sizeof(body), "Threat level is now %s, score %u.",
                 headline, headline_score);
        urgency = strcmp(headline, "alert") == 0 ? OBSERVORE_URGENCY_URGENT
                                                 : OBSERVORE_URGENCY_NORMAL;
    } else {
        observore_digest_build(entries, count,
                               headline[0] ? headline : NULL,
                               title, sizeof(title), body, sizeof(body));
    }

    /* Appended rather than ranked among the findings: an available update is
     * not a thing that was detected, and it must never displace one that was.
     * Dropped silently if the body is already full, because the console carries
     * the same information and a truncated finding would not. */
    if (update_version[0]) {
        size_t used = strlen(body);
        int need = snprintf(NULL, 0, "\n%s available", update_version);
        if (used + (size_t)need + 1 < sizeof(body)) {
            snprintf(body + used, sizeof(body) - used, "\n%s available",
                     update_version);
        }
    }

    if (send_now(title, body, urgency) != ESP_OK) {
        s_failed++;
        s_consecutive_failures++;
        s_backoff_us = s_backoff_us ? s_backoff_us * 2 : BACKOFF_MIN_US;
        if (s_backoff_us > BACKOFF_MAX_US) {
            s_backoff_us = BACKOFF_MAX_US;
        }
        s_retry_after_us = esp_timer_get_time() + s_backoff_us;
        /* Only the first failure of a run is worth a line. The rest say the
         * same thing, and a log that repeats itself every couple of seconds for
         * seven hours buries everything else the device has to say. */
        if (s_consecutive_failures == 1) {
            ESP_LOGW(TAG, "push failed: %s -- retrying in %llds",
                     s_last_error, (long long)(s_backoff_us / 1000000));
        } else {
            ESP_LOGD(TAG, "push failed again (%" PRIu32 " in a row)",
                     s_consecutive_failures);
        }
        return;   /* everything stays queued for the next pump */
    }

    if (s_consecutive_failures) {
        ESP_LOGI(TAG, "notifier reachable again after %" PRIu32 " failures",
                 s_consecutive_failures);
    }
    s_consecutive_failures = 0;
    s_backoff_us = 0;
    s_retry_after_us = 0;

    LOCK();
    /* Drop only what was actually in the digest. Anything detected while the
     * message was in flight is left for the next one rather than discarded. */
    size_t sent = queued < s_count ? queued : s_count;
    s_head = (s_head + sent) % OBSERVORE_NOTIFY_QUEUE;
    s_count -= sent;
    s_headline[0] = '\0';
    s_headline_score = 0;
    /* Only if it was the version this message actually carried: a check that
     * found a newer one while this was in flight must still be announced. */
    if (strcmp(s_update_version, update_version) == 0) {
        s_update_version[0] = '\0';
    }
    UNLOCK();
    s_sent++;
}

esp_err_t observore_notify_test(void)
{
    if (!observore_notify_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = send_now("Observore test",
                             "Notifications are working. Sent from the console.",
                             OBSERVORE_URGENCY_NORMAL);
    if (err == ESP_OK) {
        s_sent++;
    } else {
        s_failed++;
    }
    return err;
}

#else /* !CONFIG_OBSERVORE_NOTIFIER */

/* Built without a notifier. Every entry point is here so the rest of the
 * firmware does not have to know, and each does the least surprising nothing:
 * queues are empty, nothing is configured, and asking to configure something
 * says so rather than pretending. */
void observore_notify_init(void) {}
bool observore_notify_configured(void) { return false; }
bool observore_notify_url(char *out, size_t len) { if (out && len) out[0] = '\0'; return false; }
esp_err_t observore_notify_set(observore_provider_t p, const char *u, const char *t, const char *s)
{ (void)p; (void)u; (void)t; (void)s; return ESP_ERR_NOT_SUPPORTED; }
observore_provider_t observore_notify_provider(void) { return OBSERVORE_PROVIDER_GOTIFY; }
bool observore_notify_has_user(void) { return false; }
esp_err_t observore_notify_clear(void) { return ESP_OK; }
void observore_notify_event(const observore_event_t *ev) { (void)ev; }
void observore_notify_level(observore_level_t f, observore_level_t t, uint16_t s) { (void)f; (void)t; (void)s; }
void observore_notify_update_available(const char *v) { (void)v; }
void observore_notify_pump(void) {}
esp_err_t observore_notify_test(void) { return ESP_ERR_NOT_SUPPORTED; }
uint32_t observore_notify_sent(void) { return 0; }
uint32_t observore_notify_failed(void) { return 0; }
uint32_t observore_notify_dropped(void) { return 0; }
uint32_t observore_notify_retry_in_s(void) { return 0; }
size_t observore_notify_pending(void) { return 0; }
const char *observore_notify_last_error(void) { return ""; }

#endif /* CONFIG_OBSERVORE_NOTIFIER */
