#include "observore_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "observore_version.h"
#include "observore_notify.h"
#include "observore_wifi.h"

static const char *TAG = "observore.update";

#define HTTP_TIMEOUT_MS 5000
/* The published manifest is under a kilobyte. Twice that leaves room for the
 * file to grow a board or two without this quietly starting to fail, and still
 * costs less than the TLS session it arrives over. */
#define DOC_MAX 2048

/* Retry sooner than the normal interval when a check fails, but not so soon
 * that an unreachable host is retried every window -- the same reasoning as the
 * notifier's backoff, for the same reason. */
#define RETRY_AFTER_US (30ULL * 60 * 1000000)

static char     s_latest[24];
static char     s_error[64];
static int64_t  s_last_ok_us;
static int64_t  s_next_check_us;
static bool     s_available;
static char     s_doc[DOC_MAX];

void observore_update_init(void)
{
    s_latest[0]     = '\0';
    s_error[0]      = '\0';
    s_last_ok_us    = 0;
    /* Not at boot: the first uplink window has notifications to send and a
     * clock to set, and an update that has waited for a release can wait a few
     * minutes more. */
    s_next_check_us = esp_timer_get_time() + (5ULL * 60 * 1000000);
    s_available     = false;
}

const char *observore_update_running_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return app && app->version[0] ? app->version : "unknown";
}

const char *observore_update_latest_version(void) { return s_latest; }
bool        observore_update_available(void)      { return s_available; }
const char *observore_update_error(void)          { return s_error; }

long observore_update_age_s(void)
{
    if (s_last_ok_us == 0) {
        return -1;
    }
    return (long)((esp_timer_get_time() - s_last_ok_us) / 1000000);
}

/* Read the whole document into one buffer.
 *
 * esp_http_client_read_response() is used rather than the event callback
 * because the document is small and known, and a callback would have to
 * reassemble it across chunks anyway. */
static esp_err_t fetch(char *out, size_t out_len)
{
    esp_http_client_config_t cfg = {
        .url               = CONFIG_OBSERVORE_UPDATE_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(s_error, sizeof(s_error), "out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "%s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    int64_t len = esp_http_client_fetch_headers(c);
    int status  = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(s_error, sizeof(s_error), "HTTP %d", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    if (len > 0 && (size_t)len >= out_len) {
        /* Refuse rather than read a prefix: half a document parses into a
         * plausible-looking version as readily as a whole one. */
        snprintf(s_error, sizeof(s_error), "manifest too large (%lld bytes)",
                 (long long)len);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }

    int got = esp_http_client_read_response(c, out, (int)out_len - 1);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (got <= 0) {
        snprintf(s_error, sizeof(s_error), "empty response");
        return ESP_FAIL;
    }
    out[got] = '\0';
    return ESP_OK;
}

void observore_update_check(void)
{
    if (observore_wifi_mode() != OBSERVORE_MODE_UPLINK) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_next_check_us) {
        return;
    }

    if (fetch(s_doc, sizeof(s_doc)) != ESP_OK) {
        ESP_LOGW(TAG, "update check failed: %s", s_error);
        s_next_check_us = now + RETRY_AFTER_US;
        return;
    }

    char found[sizeof(s_latest)];
    if (!observore_json_string_field(s_doc, "version", found, sizeof(found))) {
        snprintf(s_error, sizeof(s_error), "no version in the manifest");
        ESP_LOGW(TAG, "update check failed: %s", s_error);
        s_next_check_us = now + RETRY_AFTER_US;
        return;
    }

    s_error[0]   = '\0';
    s_last_ok_us = now;
    s_next_check_us = now +
        (int64_t)CONFIG_OBSERVORE_UPDATE_CHECK_HOURS * 3600 * 1000000LL;

    const char *running = observore_update_running_version();
    bool was = s_available;
    snprintf(s_latest, sizeof(s_latest), "%s", found);
    s_available = observore_version_is_newer(s_latest, running);

    if (s_available && !was) {
        ESP_LOGW(TAG, "%s is available; running %s", s_latest, running);
        observore_notify_update_available(s_latest);
    } else if (!s_available) {
        ESP_LOGI(TAG, "up to date on %s", running);
    }
}
