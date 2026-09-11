#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argus_mute.h"
#include "argus_netcfg.h"
#include "argus_track.h"
#include "argus_web.h"
#include "argus_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "argus.web";

static httpd_handle_t s_server;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* Scratch space for building responses.  This MUST NOT live in internal RAM.
 *
 * It was originally two static snapshots plus two static JSON buffers -- 84 KB
 * of a device that has about 180 KB of DRAM, on top of the 20 KB device table.
 * Wi-Fi and lwip were left starved: internal free heap fell to 1.4 KB with a
 * largest free block of 768 bytes, at which point the SoftAP could still beacon
 * but could no longer allocate a buffer to answer an ARP request.  The console
 * loaded once after boot and then went dead, looking for all the world like a
 * network problem.
 *
 * The buffers now come from PSRAM, of which there are 8 MB doing nothing, and
 * are shared between handlers: esp_http_server dispatches requests from a
 * single task, so only one handler is ever building a response. */
#define JSON_BUF_LEN (32 * 1024)

/* Both are heap pointers, so sizeof() on them yields 4, not the buffer size.
 * Always bound writes with JSON_BUF_LEN -- a missed conversion here silently
 * truncated /api/devices, which the browser then refused to parse. */
static argus_event_t *s_snap;    /* ARGUS_MAX_DEVICES entries */
static char          *s_body;    /* JSON_BUF_LEN bytes */

static bool scratch_alloc(void)
{
    if (s_snap && s_body) {
        return true;
    }
    s_snap = heap_caps_malloc(sizeof(argus_event_t) * ARGUS_MAX_DEVICES,
                              MALLOC_CAP_SPIRAM);
    s_body = heap_caps_malloc(JSON_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (!s_snap || !s_body) {
        /* Without PSRAM there is no safe place for this, so refuse to start
         * rather than quietly starve the network stack again. */
        ESP_LOGE(TAG, "could not allocate %d KB of PSRAM scratch",
                 (int)((sizeof(argus_event_t) * ARGUS_MAX_DEVICES +
                        JSON_BUF_LEN) / 1024));
        free(s_snap);
        free(s_body);
        s_snap = NULL;
        s_body = NULL;
        return false;
    }
    return true;
}

static void scratch_free(void)
{
    free(s_snap);
    free(s_body);
    s_snap = NULL;
    s_body = NULL;
}

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
                     ",\"mode\":\"%s\",\"muted\":%zu,"
                     "\"suppressed\":%" PRIu32 ",\"counts\":{",
                     st.score, argus_level_name(st.level), st.device_count,
                     st.total_sightings, now / 1000000,
                     argus_mode_name(argus_wifi_mode()),
                     argus_mute_count(), argus_mute_suppressed());

    for (int c = 1; c < ARGUS_CLASS_MAX && n < (int)sizeof(body); c++) {
        n += snprintf(body + n, sizeof(body) - n, "%s\"%s\":%" PRIu32,
                      c > 1 ? "," : "", argus_class_name(c), st.class_counts[c]);
    }
    snprintf(body + n, sizeof(body) - n, "}}");

    return send_json(req, body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    argus_event_t *snap = s_snap;
    char *body = s_body;

    int64_t now = esp_timer_get_time();
    size_t count = argus_track_snapshot(snap, ARGUS_MAX_DEVICES, now);

    int n = snprintf(body, JSON_BUF_LEN, "{\"devices\":[");
    for (size_t i = 0; i < count; i++) {
        const argus_event_t *e = &snap[i];
        char detail[sizeof(e->detail) * 2 + 1];
        char label[sizeof(e->label) * 2 + 1];
        json_escape(e->detail, detail, sizeof(detail));
        json_escape(e->label, label, sizeof(label));
        const char *vendor = e->vendor ? e->vendor : "";

        int written = snprintf(
            body + n, JSON_BUF_LEN - n,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"class\":\"%s\","
            "\"label\":\"%s\",\"vendor\":\"%s\",\"random\":%s,"
            "\"detail\":\"%s\",\"evidence\":\"%s\","
            "\"source\":\"%s\",\"rssi\":%d,\"channel\":%u,\"hits\":%" PRIu32 ","
            "\"first_seen_s\":%" PRId64 ",\"last_seen_s\":%" PRId64 "}",
            i ? "," : "",
            e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            argus_class_name(e->cls), label, vendor,
            e->addr_random ? "true" : "false", detail,
            argus_evidence_name(e->evidence), argus_source_name(e->src),
            e->rssi, e->channel, e->hits,
            (now - e->first_seen_us) / 1000000,
            (now - e->last_seen_us) / 1000000);

        if (written < 0 || n + written >= JSON_BUF_LEN - 4) {
            /* Out of room: close the array honestly rather than emitting
             * truncated JSON the browser cannot parse. */
            ESP_LOGW(TAG, "device list truncated at %zu of %zu", i, count);
            break;
        }
        n += written;
    }
    snprintf(body + n, JSON_BUF_LEN - n, "]}");

    return send_json(req, body);
}

/* Read one query parameter.  Returns false when absent. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, out, len) != ESP_OK) {
        return false;
    }
    /* Values arrive percent-encoded; SSIDs routinely contain spaces. */
    char *w = out;
    for (char *r = out; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = (r[1] <= '9') ? r[1] - '0' : (r[1] | 0x20) - 'a' + 10;
            int lo = (r[2] <= '9') ? r[2] - '0' : (r[2] | 0x20) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *w++ = (char)((hi << 4) | lo);
                r += 2;
            } else {
                *w++ = *r;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
    return true;
}

static esp_err_t fail(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    char body[160];
    char safe[96];
    json_escape(why, safe, sizeof(safe));
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", safe);
    return send_json(req, body);
}

static esp_err_t mutes_handler(httpd_req_t *req)
{
    static argus_mute_rule_t rules[ARGUS_MUTE_MAX];
    size_t n = argus_mute_list(rules, ARGUS_MUTE_MAX);

    char body[4096];
    int w = snprintf(body, sizeof(body), "{\"suppressed\":%" PRIu32 ",\"rules\":[",
                     argus_mute_suppressed());
    for (size_t i = 0; i < n; i++) {
        const argus_mute_rule_t *r = &rules[i];
        char value[ARGUS_MUTE_SSID_LEN * 2];

        switch (r->kind) {
            case ARGUS_MUTE_MAC:
                snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
                         r->mac[0], r->mac[1], r->mac[2],
                         r->mac[3], r->mac[4], r->mac[5]);
                break;
            case ARGUS_MUTE_OUI:
                snprintf(value, sizeof(value), "%02X:%02X:%02X",
                         r->mac[0], r->mac[1], r->mac[2]);
                break;
            case ARGUS_MUTE_CLASS:
                snprintf(value, sizeof(value), "%s", argus_class_name(r->cls));
                break;
            default:
                json_escape(r->ssid, value, sizeof(value));
                break;
        }
        w += snprintf(body + w, sizeof(body) - w,
                      "%s{\"index\":%zu,\"kind\":\"%s\",\"value\":\"%s\"}",
                      i ? "," : "", i, argus_mute_kind_name(r->kind), value);
    }
    snprintf(body + w, sizeof(body) - w, "]}");
    return send_json(req, body);
}

static esp_err_t mute_handler(httpd_req_t *req)
{
    argus_mute_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    char value[ARGUS_MUTE_SSID_LEN];

    if (query_param(req, "mac", value, sizeof(value))) {
        if (!argus_mute_parse_mac(value, rule.mac, ARGUS_MAC_LEN)) {
            return fail(req, "mac must be AA:BB:CC:DD:EE:FF");
        }
        rule.kind = ARGUS_MUTE_MAC;
    } else if (query_param(req, "oui", value, sizeof(value))) {
        if (!argus_mute_parse_mac(value, rule.mac, 3)) {
            return fail(req, "oui must be AA:BB:CC");
        }
        rule.kind = ARGUS_MUTE_OUI;
    } else if (query_param(req, "class", value, sizeof(value))) {
        argus_class_t cls;
        if (!argus_mute_parse_class(value, &cls)) {
            return fail(req, "unknown class");
        }
        rule.kind = ARGUS_MUTE_CLASS;
        rule.cls = (uint8_t)cls;
    } else if (query_param(req, "ssid", value, sizeof(value))) {
        rule.kind = ARGUS_MUTE_SSID;
        snprintf(rule.ssid, sizeof(rule.ssid), "%s", value);
    } else {
        return fail(req, "expected one of mac, oui, class, ssid");
    }

    esp_err_t err = argus_mute_add(&rule);
    if (err == ESP_ERR_NO_MEM) {
        return fail(req, "mute list is full");
    }
    if (err != ESP_OK) {
        return fail(req, "invalid rule");
    }
    ESP_LOGI(TAG, "muted %s", argus_mute_kind_name(rule.kind));
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t unmute_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "all", value, sizeof(value))) {
        argus_mute_clear();
        return send_json(req, "{\"ok\":true}");
    }
    if (!query_param(req, "index", value, sizeof(value))) {
        return fail(req, "expected index or all=1");
    }
    if (argus_mute_remove((size_t)strtoul(value, NULL, 10)) != ESP_OK) {
        return fail(req, "no such rule");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t netcfg_get_handler(httpd_req_t *req)
{
    char ssid[ARGUS_SSID_LEN] = {0};
    bool set = argus_netcfg_ssid(ssid, sizeof(ssid));
    char escaped[ARGUS_SSID_LEN * 2];
    json_escape(ssid, escaped, sizeof(escaped));

    char body[256];
    /* The password is deliberately absent and there is no endpoint that can
     * read it back.  It is write-only from outside the device. */
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"ssid\":\"%s\",\"mode\":\"%s\","
             "\"ip\":\"%s\"}",
             set ? "true" : "false", escaped,
             argus_mode_name(argus_wifi_mode()), argus_wifi_uplink_ip());
    return send_json(req, body);
}

static esp_err_t netcfg_set_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "clear", value, sizeof(value))) {
        argus_netcfg_clear();
        ESP_LOGI(TAG, "network credentials cleared");
        return send_json(req, "{\"ok\":true}");
    }

    char ssid[ARGUS_SSID_LEN] = {0};
    char password[ARGUS_PASSWORD_LEN] = {0};
    if (!query_param(req, "ssid", ssid, sizeof(ssid))) {
        return fail(req, "ssid is required");
    }
    query_param(req, "password", password, sizeof(password));

    const char *why = NULL;
    if (!argus_netcfg_valid(ssid, password, &why)) {
        return fail(req, why);
    }
    esp_err_t err = argus_netcfg_set(ssid, password);
    /* Do not leave the password sitting on this task's stack. */
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        return fail(req, "could not store credentials");
    }
    ESP_LOGI(TAG, "network set to \"%s\"", ssid);
    return send_json(req, "{\"ok\":true}");
}

/* Unclassified devices, so known gear can be recognised and muted before it
 * ever trips the follower heuristic. */
#define NEARBY_MAX 40

static esp_err_t nearby_handler(httpd_req_t *req)
{
    argus_event_t *snap = s_snap;
    char *body = s_body;

    int64_t now = esp_timer_get_time();
    size_t count = argus_track_nearby(snap, ARGUS_MAX_DEVICES, now);
    if (count > NEARBY_MAX) {
        count = NEARBY_MAX;
    }

    int n = snprintf(body, JSON_BUF_LEN, "{\"nearby\":[");
    for (size_t i = 0; i < count; i++) {
        const argus_event_t *e = &snap[i];
        /* The name is chosen by whoever owns the radio, so it is escaped on
         * the way out exactly like every other remote-controlled string. */
        char name[sizeof(e->detail) * 2 + 1];
        json_escape(e->detail, name, sizeof(name));

        int written = snprintf(
            body + n, JSON_BUF_LEN - n,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"vendor\":\"%s\","
            "\"name\":\"%s\",\"random\":%s,\"source\":\"%s\","
            "\"rssi\":%d,\"hits\":%" PRIu32 ",\"last_seen_s\":%" PRId64 "}",
            i ? "," : "",
            e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            e->vendor ? e->vendor : "", name,
            e->addr_random ? "true" : "false", argus_source_name(e->src),
            e->rssi, e->hits, (now - e->last_seen_us) / 1000000);
        if (written < 0 || n + written >= JSON_BUF_LEN - 4) {
            break;
        }
        n += written;
    }
    snprintf(body + n, JSON_BUF_LEN - n, "]}");
    return send_json(req, body);
}

/* Mark everything currently in range as known, then start watching for what
 * changes from here.  Both classified and unclassified devices are muted: at
 * home your own doorbell camera is exactly the thing you want silenced, and
 * the action is fully reversible from the same panel. */
static esp_err_t baseline_handler(httpd_req_t *req)
{
    argus_event_t *snap = s_snap;
    size_t count = argus_track_all(snap, ARGUS_MAX_DEVICES);

    size_t added = 0, existing = 0, full = 0, rotating = 0;
    for (size_t i = 0; i < count; i++) {
        argus_mute_rule_t rule = {.kind = ARGUS_MUTE_MAC};
        memcpy(rule.mac, snap[i].mac, ARGUS_MAC_LEN);

        size_t before = argus_mute_count();
        esp_err_t err = argus_mute_add(&rule);
        if (err == ESP_ERR_NO_MEM) {
            full++;
        } else if (err != ESP_OK) {
            continue;
        } else if (argus_mute_count() == before) {
            existing++;
        } else {
            added++;
            /* A rotating address will be back under a different MAC within
             * the hour, so the rule covering it is temporary.  Counted so the
             * UI can say so rather than implying a permanent result. */
            if (snap[i].addr_random) {
                rotating++;
            }
        }
    }

    /* Everything in range is now known, so the score and the log start from
     * a clean slate -- that is what makes it a baseline rather than just a
     * bulk mute. */
    argus_track_clear();

    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"seen\":%zu,\"added\":%zu,\"already\":%zu,"
             "\"rotating\":%zu,\"no_room\":%zu,\"rules\":%zu}",
             count, added, existing, rotating, full, argus_mute_count());
    ESP_LOGI(TAG, "baseline: %zu seen, %zu muted (%zu rotating), %zu no room",
             count, added, rotating, full);
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

    static const httpd_uri_t routes[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = index_handler},
        {.uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/devices",  .method = HTTP_GET,  .handler = devices_handler},
        {.uri = "/api/nearby",   .method = HTTP_GET,  .handler = nearby_handler},
        {.uri = "/api/clear",    .method = HTTP_POST, .handler = clear_handler},
        {.uri = "/api/mutes",    .method = HTTP_GET,  .handler = mutes_handler},
        {.uri = "/api/mute",     .method = HTTP_POST, .handler = mute_handler},
        {.uri = "/api/unmute",   .method = HTTP_POST, .handler = unmute_handler},
        {.uri = "/api/baseline", .method = HTTP_POST, .handler = baseline_handler},
        {.uri = "/api/netcfg",   .method = HTTP_GET,  .handler = netcfg_get_handler},
        {.uri = "/api/netcfg",   .method = HTTP_POST, .handler = netcfg_set_handler},
    };
    if (!scratch_alloc()) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;   /* the JSON handlers are not frugal */
    /* Sized from the table rather than left at the default of 8.  Overflowing
     * it makes httpd_register_uri_handler fail and the route simply not exist,
     * which surfaces as a 405 on a route that is plainly in the source. */
    cfg.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        scratch_free();
        return err;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            /* Never silently serve a partial API. */
            ESP_LOGE(TAG, "could not register %s: %s", routes[i].uri,
                     esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            scratch_free();
            return err;
        }
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
    scratch_free();
    return err;
}
