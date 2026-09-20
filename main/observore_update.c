#include "observore_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "observore_version.h"
#include "observore_ble.h"
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
static bool     s_check_pending;
static bool     s_check_retried;
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

void observore_update_check_now(void)
{
    s_check_pending = true;
    s_check_retried = false;
    s_next_check_us = 0;
}

bool observore_update_check_pending(void) { return s_check_pending; }

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
        /* A check someone asked for gets one more go a few seconds on. On
         * the bench the first attempt seven seconds after the uplink came up
         * failed to connect and the next, seconds later, succeeded; a button
         * that answers "could not connect" to that is answering a question
         * nobody asked. The daily check keeps its half-hour retry. */
        if (s_check_pending && !s_check_retried) {
            s_check_retried = true;
            s_next_check_us = now + (5LL * 1000000);
            return;
        }
        s_check_pending = false;
        s_next_check_us = now + RETRY_AFTER_US;
        return;
    }
    s_check_pending = false;

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

/* ------------------------------------------------------------------ */
/* Installing                                                         */
/* ------------------------------------------------------------------ */

static observore_update_state_t s_state;
static int      s_progress = -1;
static char     s_image_url[192];

const char *observore_update_state_name(observore_update_state_t s)
{
    switch (s) {
        case OBSERVORE_UPDATE_IDLE:      return "idle";
        case OBSERVORE_UPDATE_REQUESTED: return "requested";
        case OBSERVORE_UPDATE_RUNNING:   return "running";
        case OBSERVORE_UPDATE_FAILED:    return "failed";
        case OBSERVORE_UPDATE_REBOOTING: return "rebooting";
    }
    return "?";
}

observore_update_state_t observore_update_state(void) { return s_state; }
int observore_update_progress(void)                   { return s_progress; }

/* The image comes from the release asset host, named for the version and the
 * board.
 *
 * Not from beside the manifest, which would be the obvious choice and is wrong.
 * The flasher site is behind a CDN that compresses this file, then answers a
 * range request against the compressed copy while serving the decompressed one:
 * a request for bytes 0-4095 comes back with 10,602 bytes and a total of
 * 906,227 against a real size of 1,447,536. An image assembled from those
 * offsets is not the image. The release host answers the same request with
 * exactly 4096 bytes and the true total. */
static bool build_image_url(char *out, size_t out_len)
{
    if (s_latest[0] == '\0') {
        return false;
    }
    int n = snprintf(out, out_len, "%s/%s/observore-%s.bin",
                     CONFIG_OBSERVORE_UPDATE_ASSET_BASE, s_latest,
                     CONFIG_OBSERVORE_BOARD);
    return n > 0 && (size_t)n < out_len;
}

esp_err_t observore_update_install(void)
{
    if (CONFIG_OBSERVORE_BOARD[0] == '\0') {
        /* A device that does not know which board it is cannot know which image
         * is its own, and two of the boards this runs on share a chip. Stopping
         * is the only safe answer. */
        snprintf(s_error, sizeof(s_error), "this build does not name its board");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_available) {
        snprintf(s_error, sizeof(s_error), "nothing newer to install");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_state == OBSERVORE_UPDATE_RUNNING ||
        s_state == OBSERVORE_UPDATE_REBOOTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_image_url(s_image_url, sizeof(s_image_url))) {
        snprintf(s_error, sizeof(s_error), "cannot build an image URL");
        return ESP_ERR_INVALID_ARG;
    }
    s_error[0]  = '\0';
    s_progress  = -1;
    s_state     = OBSERVORE_UPDATE_REQUESTED;
    ESP_LOGW(TAG, "update to %s requested", s_latest);
    return ESP_OK;
}

/* Refuse an image that is not ours or not newer.
 *
 * The descriptor is read from the first chunk, before anything is written to
 * flash, so a wrong image costs a few kilobytes of download and nothing else.
 * Worth doing even though the URL was built from the board name: a misconfigured
 * update host is exactly the case where the URL cannot be trusted to say what
 * the bytes are. */
static bool image_is_acceptable(esp_https_ota_handle_t h)
{
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "no descriptor in the image");
        return false;
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (self && strncmp(desc.project_name, self->project_name,
                        sizeof(desc.project_name)) != 0) {
        snprintf(s_error, sizeof(s_error), "image is \"%.16s\", not ours",
                 desc.project_name);
        return false;
    }
    if (!observore_version_is_newer(desc.version,
                                    observore_update_running_version())) {
        snprintf(s_error, sizeof(s_error), "image %.16s is not newer",
                 desc.version);
        return false;
    }
    ESP_LOGI(TAG, "image says %s", desc.version);
    return true;
}

/* Give up on a download and get the detector back.
 *
 * BLE was torn down to free the memory the TLS session needed, and bringing the
 * controller back up in place is more moving parts on the one path that only
 * runs when something has already gone wrong. A restart is the simple, total
 * recovery: the device comes back detecting on both radios, the old image is
 * still the one that boots, and the reason is in the log above this line.
 *
 * The cost is that the console loses the failure state it was showing. The log
 * keeps it, and a device that quietly stopped watching Bluetooth would be a
 * worse thing to leave behind than a reboot. */
static void abandon_update(void)
{
    s_state    = OBSERVORE_UPDATE_FAILED;
    s_progress = -1;
    ESP_LOGE(TAG, "update abandoned (%s); restarting to bring BLE back",
             s_error[0] ? s_error : "no reason recorded");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void observore_update_service(void)
{
    if (s_state != OBSERVORE_UPDATE_REQUESTED) {
        return;
    }
    if (observore_wifi_mode() != OBSERVORE_MODE_UPLINK ||
        !observore_wifi_uplink_connected()) {
        return;                      /* wait for a window; the request stands */
    }

    s_state = OBSERVORE_UPDATE_RUNNING;
    ESP_LOGW(TAG, "downloading %s", s_image_url);

    /* Free what the BLE stack is holding before asking for a TLS session.
     *
     * The sniffer is already suspended on the uplink, so nothing is being
     * detected during a download either way. What matters is the memory: the
     * controller's buffers are DMA-capable internal RAM, the hardware AES driver
     * needs the same kind, and MBEDTLS_EXTERNAL_MEM_ALLOC cannot move that to
     * PSRAM. With the stack resident the handshake fails on "esp-aes: Failed to
     * allocate memory" before a byte is downloaded. */
    unsigned before = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    observore_ble_stop();
    ESP_LOGI(TAG, "stopped BLE for the download: internal heap %u -> %u bytes",
             before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_http_client_config_t http = {
        .url               = s_image_url,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        /* The release host answers with a redirect to a signed URL that runs to
         * several hundred characters, and the client's default 512-byte buffers
         * cannot hold it -- it fails with "Out of buffer" before the download
         * starts. Both directions need the room: the Location header arrives in
         * the receive buffer and the request line that follows carries the same
         * URL back out. Charged only while an update is downloading, which is
         * also when the BLE scan is stopped. */
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        /* Fetch the image in ranged pieces rather than one long stream.
         *
         * Not a throughput choice. MBEDTLS_SSL_IN_CONTENT_LEN is 8192 here,
         * halved from the default to survive the memory pressure that once cost
         * 10,019 notifications, and a TLS record may carry 16 KB. A CDN serving
         * a 1.4 MB file fills its records, and the first full one fails the read
         * with MBEDTLS_ERR_SSL_BAD_INPUT_DATA (-0x7100) -- which is what this
         * did before the range requests went in. Small responses, like every
         * notification and the manifest, fit and always did.
         *
         * Asking for a bounded range keeps each response inside the buffer we
         * can afford, at the cost of one request per piece. */
        .partial_http_download = true,
        .max_http_request_size  = 4096,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        snprintf(s_error, sizeof(s_error), "%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "update failed to start: %s", s_error);
        abandon_update();
        return;
    }

    if (!image_is_acceptable(h)) {
        ESP_LOGE(TAG, "refusing the image: %s", s_error);
        esp_https_ota_abort(h);
        abandon_update();
        return;
    }

    int total = esp_https_ota_get_image_size(h);
    int last_logged = -10;
    while ((err = esp_https_ota_perform(h)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        s_progress = (total > 0) ? (int)((int64_t)done * 100 / total) : -1;
        if (s_progress >= last_logged + 10) {
            last_logged = s_progress;
            ESP_LOGI(TAG, "update %d%% (%d of %d bytes)", s_progress, done, total);
        }
    }

    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "download failed: %s", s_error);
        esp_https_ota_abort(h);
        abandon_update();
        return;
    }

    /* finish() verifies the image and sets the boot partition. Its own
     * validation failure is reported separately from a transport one, because
     * they mean very different things: a bad image on the server, against a
     * connection that dropped. */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "image rejected: %s", s_error);
        abandon_update();
        return;
    }

    s_progress = 100;
    s_state    = OBSERVORE_UPDATE_REBOOTING;
    ESP_LOGW(TAG, "update written; restarting into %s", s_latest);
    /* Long enough for the console to see the state and for the log to flush,
     * short enough that nobody wonders whether it worked. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void observore_update_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                       /* not on probation; nothing to confirm */
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "this image has proved itself; rollback cancelled");
    }
}
