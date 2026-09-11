#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "argus_track.h"
#include "argus_web.h"
#include "argus_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "argus.web";

static httpd_handle_t s_server;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* The page is served from flash and the JSON is built on the stack, so the
 * response buffer is the only sizeable allocation here.  192 devices at ~150
 * bytes of JSON each needs room to spare. */
#define JSON_BUF_LEN (32 * 1024)

/* Escape a string for embedding in JSON.  Inputs here are advertised names and
 * SSIDs -- remote-controlled bytes -- so this is not optional.  The upstream
 * parsers already strip non-printable characters; this handles the rest. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c >= 0x20 && c < 0x7F) {
            out[o++] = c;
        } else {
            /* Anything else becomes a space rather than a broken escape. */
            out[o++] = ' ';
        }
    }
    out[o] = '\0';
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    argus_status_t st;
    argus_track_status(&st, now);

    char body[768];
    int n = snprintf(body, sizeof(body),
                     "{\"score\":%u,\"level\":\"%s\",\"devices\":%u,"
                     "\"sightings\":%" PRIu32 ",\"uptime_s\":%" PRId64
                     ",\"mode\":\"%s\",\"counts\":{",
                     st.score, argus_level_name(st.level), st.device_count,
                     st.total_sightings, now / 1000000,
                     argus_wifi_mode() == ARGUS_MODE_CONSOLE ? "console" : "patrol");

    for (int c = 1; c < ARGUS_CLASS_MAX && n < (int)sizeof(body); c++) {
        n += snprintf(body + n, sizeof(body) - n, "%s\"%s\":%" PRIu32,
                      c > 1 ? "," : "", argus_class_name(c), st.class_counts[c]);
    }
    snprintf(body + n, sizeof(body) - n, "}}");

    return send_json(req, body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    static argus_event_t snap[ARGUS_MAX_DEVICES];
    static char body[JSON_BUF_LEN];

    int64_t now = esp_timer_get_time();
    size_t count = argus_track_snapshot(snap, ARGUS_MAX_DEVICES, now);

    int n = snprintf(body, sizeof(body), "{\"devices\":[");
    for (size_t i = 0; i < count; i++) {
        const argus_event_t *e = &snap[i];
        char detail[sizeof(e->detail) * 2 + 1];
        char label[sizeof(e->label) * 2 + 1];
        json_escape(e->detail, detail, sizeof(detail));
        json_escape(e->label, label, sizeof(label));

        int written = snprintf(
            body + n, sizeof(body) - n,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"class\":\"%s\","
            "\"label\":\"%s\",\"detail\":\"%s\",\"evidence\":\"%s\","
            "\"source\":\"%s\",\"rssi\":%d,\"channel\":%u,\"hits\":%" PRIu32 ","
            "\"first_seen_s\":%" PRId64 ",\"last_seen_s\":%" PRId64 "}",
            i ? "," : "",
            e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            argus_class_name(e->cls), label, detail,
            argus_evidence_name(e->evidence), argus_source_name(e->src),
            e->rssi, e->channel, e->hits,
            (now - e->first_seen_us) / 1000000,
            (now - e->last_seen_us) / 1000000);

        if (written < 0 || n + written >= (int)sizeof(body) - 4) {
            /* Out of room: close the array honestly rather than emitting
             * truncated JSON the browser cannot parse. */
            ESP_LOGW(TAG, "device list truncated at %zu of %zu", i, count);
            break;
        }
        n += written;
    }
    snprintf(body + n, sizeof(body) - n, "]}");

    return send_json(req, body);
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    argus_track_clear();
    ESP_LOGI(TAG, "log cleared by console");
    return send_json(req, "{\"ok\":true}");
}

esp_err_t argus_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;   /* the JSON handlers are not frugal */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/devices",  .method = HTTP_GET,  .handler = devices_handler},
        {.uri = "/api/clear",    .method = HTTP_POST, .handler = clear_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "console at http://192.168.4.1/ (SSID %s)", argus_wifi_ap_ssid());
    return ESP_OK;
}

esp_err_t argus_web_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
