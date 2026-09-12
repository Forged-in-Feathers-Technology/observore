#include <stdio.h>
#include <string.h>

#include "observore_notify.h"
#include "observore_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "observore.notify";

#include "observore_nvs.h"
#define NVS_NAMESPACE OBSERVORE_NVS_NAMESPACE
#define NVS_KEY_URL   "gotify_url"
#define NVS_KEY_TOKEN "gotify_tok"

/* Sending is done from the main loop, so a slow or unreachable server would
 * otherwise stall detection for the full TCP timeout. */
#define HTTP_TIMEOUT_MS 5000
/* How many queued notices to flush per pump.  More than a couple in one pass
 * would hold the loop for seconds on a slow link. */
#define MAX_PER_PUMP 2

typedef struct {
    char    title[OBSERVORE_NOTIFY_TITLE_LEN];
    char    message[OBSERVORE_NOTIFY_MSG_LEN];
    uint8_t priority;
} observore_notice_t;

static SemaphoreHandle_t s_lock;
static char     s_url[OBSERVORE_NOTIFY_URL_LEN];
static char     s_token[OBSERVORE_NOTIFY_TOKEN_LEN];
static observore_notice_t s_queue[OBSERVORE_NOTIFY_QUEUE];
static size_t   s_head, s_count;
static uint32_t s_sent, s_failed, s_dropped;
static char     s_last_error[64];

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ------------------------------------------------------------------ */

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_url);
    if (nvs_get_str(h, NVS_KEY_URL, s_url, &len) != ESP_OK) {
        s_url[0] = '\0';
    }
    len = sizeof(s_token);
    if (nvs_get_str(h, NVS_KEY_TOKEN, s_token, &len) != ESP_OK) {
        s_token[0] = '\0';
    }
    nvs_close(h);
    if (s_url[0]) {
        ESP_LOGI(TAG, "notifying %s", s_url);
    }
}

static void save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, NVS_KEY_URL, s_url);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_TOKEN, s_token);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist notifier config: %s",
                 esp_err_to_name(err));
    }
}

void observore_notify_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    LOCK();
    memset(s_url, 0, sizeof(s_url));
    memset(s_token, 0, sizeof(s_token));
    s_head = s_count = 0;
    s_sent = s_failed = s_dropped = 0;
    s_last_error[0] = '\0';
    load();
    UNLOCK();
}

bool observore_notify_configured(void)
{
    LOCK();
    bool ok = s_url[0] != '\0';
    UNLOCK();
    return ok;
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

esp_err_t observore_notify_set(const char *url, const char *token)
{
    if (!url || !*url) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= OBSERVORE_NOTIFY_URL_LEN ||
        (token && strlen(token) >= OBSERVORE_NOTIFY_TOKEN_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }
    LOCK();
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    save();
    UNLOCK();
    return ESP_OK;
}

esp_err_t observore_notify_clear(void)
{
    LOCK();
    memset(s_url, 0, sizeof(s_url));
    memset(s_token, 0, sizeof(s_token));
    s_head = s_count = 0;
    save();
    UNLOCK();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Queue                                                              */
/* ------------------------------------------------------------------ */

static void enqueue(const char *title, const char *message, uint8_t priority)
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
    n->priority = priority;
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

/* Gotify priority: 8 shows as a high-priority alert on Android, 5 is a normal
 * notification, 2 is quiet. */
static uint8_t priority_for(observore_class_t cls)
{
    switch (cls) {
        case OBSERVORE_CLASS_BODYCAM:
        case OBSERVORE_CLASS_ALPR:
            return 8;
        case OBSERVORE_CLASS_FOLLOWER:
        case OBSERVORE_CLASS_TRACKER:
            return 7;
        case OBSERVORE_CLASS_DRONE:
        case OBSERVORE_CLASS_SMARTGLASSES:
            return 5;
        default:
            return 2;
    }
}

void observore_notify_event(const observore_event_t *ev)
{
    if (!ev) {
        return;
    }
    char title[OBSERVORE_NOTIFY_TITLE_LEN];
    char msg[OBSERVORE_NOTIFY_MSG_LEN];

    snprintf(title, sizeof(title), "%s detected", observore_class_name(ev->cls));
    snprintf(msg, sizeof(msg),
             "%s%s%s\n%02X:%02X:%02X:%02X:%02X:%02X %s\n%d dBm, via %s, %s",
             ev->label,
             ev->detail[0] ? " / " : "", ev->detail,
             ev->mac[0], ev->mac[1], ev->mac[2],
             ev->mac[3], ev->mac[4], ev->mac[5],
             ev->vendor ? ev->vendor : (ev->addr_random ? "(random)" : ""),
             ev->rssi, observore_source_name(ev->src),
             observore_evidence_name(ev->evidence));

    enqueue(title, msg, priority_for(ev->cls));
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
    enqueue(title, msg, to == OBSERVORE_LEVEL_ALERT ? 8 : 5);
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

/* Escape a string into a JSON body. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < out_len; i++) {
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

static esp_err_t send_now(const observore_notice_t *n)
{
    char url[OBSERVORE_NOTIFY_URL_LEN + 16];
    char token[OBSERVORE_NOTIFY_TOKEN_LEN];

    LOCK();
    size_t ulen = strlen(s_url);
    /* Tolerate a configured URL with or without a trailing slash. */
    bool slash = ulen > 0 && s_url[ulen - 1] == '/';
    snprintf(url, sizeof(url), "%s%smessage", s_url, slash ? "" : "/");
    snprintf(token, sizeof(token), "%s", s_token);
    UNLOCK();

    char title[OBSERVORE_NOTIFY_TITLE_LEN * 2];
    char message[OBSERVORE_NOTIFY_MSG_LEN * 2];
    json_escape(n->title, title, sizeof(title));
    json_escape(n->message, message, sizeof(message));

    char body[OBSERVORE_NOTIFY_TITLE_LEN * 2 + OBSERVORE_NOTIFY_MSG_LEN * 2 + 64];
    int len = snprintf(body, sizeof(body),
                       "{\"title\":\"%s\",\"message\":\"%s\",\"priority\":%u}",
                       title, message, n->priority);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (token[0]) {
        esp_http_client_set_header(c, "X-Gotify-Key", token);
    }
    esp_http_client_set_post_field(c, body, len);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    /* What matters is whether the server accepted the message.  Some servers
     * and reverse proxies answer without a Content-Length and simply close,
     * which the client reports as an incomplete read even though the POST was
     * delivered and acknowledged.  Judge by the status code when there is
     * one. */
    if (err != ESP_OK && !(status >= 200 && status < 300)) {
        snprintf(s_last_error, sizeof(s_last_error), "%s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        /* 401 here almost always means a wrong or missing application token,
         * which is worth saying plainly rather than as a bare number. */
        snprintf(s_last_error, sizeof(s_last_error), "HTTP %d%s", status,
                 status == 401 ? " (bad token)" : "");
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
    observore_notice_t n = {.priority = 5};
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
