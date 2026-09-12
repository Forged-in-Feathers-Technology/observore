#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_notify.h"
#include "observore_track.h"
#include "observore_web.h"
#include "observore_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "observore.web";

static httpd_handle_t s_server;
static int64_t        s_last_request_us;

int64_t observore_web_last_request_us(void)
{
    return s_last_request_us;
}

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
static observore_event_t *s_snap;    /* OBSERVORE_MAX_DEVICES entries */
static char          *s_body;    /* JSON_BUF_LEN bytes */

static bool scratch_alloc(void)
{
    if (s_snap && s_body) {
        return true;
    }
    s_snap = heap_caps_malloc(sizeof(observore_event_t) * OBSERVORE_MAX_DEVICES,
                              MALLOC_CAP_SPIRAM);
    s_body = heap_caps_malloc(JSON_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (!s_snap || !s_body) {
        /* Without PSRAM there is no safe place for this, so refuse to start
         * rather than quietly starve the network stack again. */
        ESP_LOGE(TAG, "could not allocate %d KB of PSRAM scratch",
                 (int)((sizeof(observore_event_t) * OBSERVORE_MAX_DEVICES +
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
    s_last_request_us = esp_timer_get_time();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    s_last_request_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    observore_status_t st;
    observore_track_status(&st, now);

    char body[768];
    int n = snprintf(body, sizeof(body),
                     "{\"score\":%u,\"level\":\"%s\",\"devices\":%u,"
                     "\"sightings\":%" PRIu32 ",\"uptime_s\":%" PRId64
                     ",\"mode\":\"%s\",\"muted\":%zu,"
                     "\"suppressed\":%" PRIu32 ",\"counts\":{",
                     st.score, observore_level_name(st.level), st.device_count,
                     st.total_sightings, now / 1000000,
                     observore_mode_name(observore_wifi_mode()),
                     observore_mute_count(), observore_mute_suppressed());

    for (int c = 1; c < OBSERVORE_CLASS_MAX && n < (int)sizeof(body); c++) {
        n += snprintf(body + n, sizeof(body) - n, "%s\"%s\":%" PRIu32,
                      c > 1 ? "," : "", observore_class_name(c), st.class_counts[c]);
    }
    snprintf(body + n, sizeof(body) - n, "}}");

    return send_json(req, body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    observore_event_t *snap = s_snap;
    char *body = s_body;

    int64_t now = esp_timer_get_time();
    size_t count = observore_track_snapshot(snap, OBSERVORE_MAX_DEVICES, now);

    int n = snprintf(body, JSON_BUF_LEN, "{\"devices\":[");
    for (size_t i = 0; i < count; i++) {
        const observore_event_t *e = &snap[i];
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
            observore_class_name(e->cls), label, vendor,
            e->addr_random ? "true" : "false", detail,
            observore_evidence_name(e->evidence), observore_source_name(e->src),
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
    static observore_mute_rule_t rules[OBSERVORE_MUTE_MAX];
    size_t n = observore_mute_list(rules, OBSERVORE_MUTE_MAX);

    char body[4096];
    int w = snprintf(body, sizeof(body), "{\"suppressed\":%" PRIu32 ",\"rules\":[",
                     observore_mute_suppressed());
    for (size_t i = 0; i < n; i++) {
        const observore_mute_rule_t *r = &rules[i];
        char value[OBSERVORE_MUTE_SSID_LEN * 2];

        switch (r->kind) {
            case OBSERVORE_MUTE_MAC:
                snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
                         r->mac[0], r->mac[1], r->mac[2],
                         r->mac[3], r->mac[4], r->mac[5]);
                break;
            case OBSERVORE_MUTE_OUI:
                snprintf(value, sizeof(value), "%02X:%02X:%02X",
                         r->mac[0], r->mac[1], r->mac[2]);
                break;
            case OBSERVORE_MUTE_CLASS:
                snprintf(value, sizeof(value), "%s", observore_class_name(r->cls));
                break;
            case OBSERVORE_MUTE_FINGERPRINT:
                snprintf(value, sizeof(value), "%08" PRIx32, r->fingerprint);
                break;
            default:
                json_escape(r->ssid, value, sizeof(value));
                break;
        }
        w += snprintf(body + w, sizeof(body) - w,
                      "%s{\"index\":%zu,\"kind\":\"%s\",\"value\":\"%s\"}",
                      i ? "," : "", i, observore_mute_kind_name(r->kind), value);
    }
    snprintf(body + w, sizeof(body) - w, "]}");
    return send_json(req, body);
}

static esp_err_t mute_handler(httpd_req_t *req)
{
    observore_mute_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    char value[OBSERVORE_MUTE_SSID_LEN];

    if (query_param(req, "mac", value, sizeof(value))) {
        if (!observore_mute_parse_mac(value, rule.mac, OBSERVORE_MAC_LEN)) {
            return fail(req, "mac must be AA:BB:CC:DD:EE:FF");
        }
        rule.kind = OBSERVORE_MUTE_MAC;
    } else if (query_param(req, "oui", value, sizeof(value))) {
        if (!observore_mute_parse_mac(value, rule.mac, 3)) {
            return fail(req, "oui must be AA:BB:CC");
        }
        rule.kind = OBSERVORE_MUTE_OUI;
    } else if (query_param(req, "class", value, sizeof(value))) {
        observore_class_t cls;
        if (!observore_mute_parse_class(value, &cls)) {
            return fail(req, "unknown class");
        }
        rule.kind = OBSERVORE_MUTE_CLASS;
        rule.cls = (uint8_t)cls;
    } else if (query_param(req, "name", value, sizeof(value)) ||
               query_param(req, "ssid", value, sizeof(value))) {
        rule.kind = OBSERVORE_MUTE_NAME;
        snprintf(rule.ssid, sizeof(rule.ssid), "%s", value);
    } else if (query_param(req, "fingerprint", value, sizeof(value))) {
        unsigned long fp = strtoul(value, NULL, 16);
        if (fp == 0 || fp > 0xFFFFFFFFUL) {
            return fail(req, "fingerprint must be non-zero hex");
        }
        rule.kind = OBSERVORE_MUTE_FINGERPRINT;
        rule.fingerprint = (uint32_t)fp;
    } else {
        return fail(req, "expected one of mac, oui, class, name, fingerprint");
    }

    esp_err_t err = observore_mute_add(&rule);
    if (err == ESP_ERR_NO_MEM) {
        return fail(req, "mute list is full");
    }
    if (err != ESP_OK) {
        return fail(req, "invalid rule");
    }
    ESP_LOGI(TAG, "muted %s", observore_mute_kind_name(rule.kind));
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t unmute_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "all", value, sizeof(value))) {
        observore_mute_clear();
        return send_json(req, "{\"ok\":true}");
    }
    if (!query_param(req, "index", value, sizeof(value))) {
        return fail(req, "expected index or all=1");
    }
    if (observore_mute_remove((size_t)strtoul(value, NULL, 10)) != ESP_OK) {
        return fail(req, "no such rule");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t netcfg_get_handler(httpd_req_t *req)
{
    char ssid[OBSERVORE_SSID_LEN] = {0};
    bool set = observore_netcfg_ssid(ssid, sizeof(ssid));
    char escaped[OBSERVORE_SSID_LEN * 2];
    json_escape(ssid, escaped, sizeof(escaped));

    char werr[224];
    json_escape(observore_wifi_uplink_error(), werr, sizeof(werr));

    char body[OBSERVORE_SSID_LEN * 2 + sizeof(werr) + 160];
    /* The password is deliberately absent and there is no endpoint that can
     * read it back.  It is write-only from outside the device. */
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"ssid\":\"%s\",\"has_password\":%s,"
             "\"mode\":\"%s\","
             "\"ip\":\"%s\",\"hostname\":\"%s\",\"error\":\"%s\"}",
             set ? "true" : "false", escaped,
             observore_netcfg_has_password() ? "true" : "false",
             observore_mode_name(observore_wifi_mode()),
             observore_wifi_uplink_ip(), observore_wifi_hostname(), werr);
    return send_json(req, body);
}

static esp_err_t netcfg_set_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "clear", value, sizeof(value))) {
        observore_netcfg_clear();
        ESP_LOGI(TAG, "network credentials cleared");
        return send_json(req, "{\"ok\":true}");
    }

    char ssid[OBSERVORE_SSID_LEN] = {0};
    char password[OBSERVORE_PASSWORD_LEN] = {0};
    if (!query_param(req, "ssid", ssid, sizeof(ssid))) {
        return fail(req, "ssid is required");
    }
    /* An absent password parameter keeps the stored one; an explicitly empty
     * one is a deliberate request for an open network. */
    bool have_password = query_param(req, "password", password,
                                     sizeof(password));

    if (have_password) {
        const char *why = NULL;
        if (!observore_netcfg_valid(ssid, password, &why)) {
            return fail(req, why);
        }
    }
    esp_err_t err = observore_netcfg_set(ssid, have_password ? password : NULL);
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
    observore_event_t *snap = s_snap;
    char *body = s_body;

    int64_t now = esp_timer_get_time();
    size_t count = observore_track_nearby(snap, OBSERVORE_MAX_DEVICES, now);
    if (count > NEARBY_MAX) {
        count = NEARBY_MAX;
    }

    int n = snprintf(body, JSON_BUF_LEN, "{\"nearby\":[");
    for (size_t i = 0; i < count; i++) {
        const observore_event_t *e = &snap[i];
        /* The name is chosen by whoever owns the radio, so it is escaped on
         * the way out exactly like every other remote-controlled string. */
        char name[sizeof(e->detail) * 2 + 1];
        json_escape(e->detail, name, sizeof(name));

        int written = snprintf(
            body + n, JSON_BUF_LEN - n,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"vendor\":\"%s\","
            "\"name\":\"%s\",\"random\":%s,\"source\":\"%s\","
            "\"fingerprint\":\"%08" PRIx32 "\","
            "\"rssi\":%d,\"hits\":%" PRIu32 ",\"last_seen_s\":%" PRId64 "}",
            i ? "," : "",
            e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            e->vendor ? e->vendor : "", name,
            e->addr_random ? "true" : "false", observore_source_name(e->src),
            e->fingerprint,
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
    observore_event_t *snap = s_snap;
    size_t count = observore_track_all(snap, OBSERVORE_MAX_DEVICES);

    size_t added = 0, existing = 0, full = 0, temporary = 0;
    size_t by_name = 0, by_fp = 0, by_mac = 0;

    for (size_t i = 0; i < count; i++) {
        const observore_event_t *e = &snap[i];
        observore_mute_rule_t rule;
        memset(&rule, 0, sizeof(rule));

        /* Pick the most durable rule this device supports.
         *
         * A name is best: it survives address rotation and is specific enough
         * to mean one device ("Encharg/492232007683").
         *
         * Otherwise a fingerprint, which also survives rotation -- but it
         * matches a KIND of device, so it is not used for the classes where
         * that could hide a real threat.
         *
         * Otherwise the MAC, which for a rotating address buys only an hour
         * or so.  Counted as temporary and reported as such. */
        if (e->detail[0] != '\0') {
            rule.kind = OBSERVORE_MUTE_NAME;
            snprintf(rule.ssid, sizeof(rule.ssid), "%s", e->detail);
        } else if (e->fingerprint != 0 &&
                   !observore_mute_class_is_protected(e->cls)) {
            rule.kind = OBSERVORE_MUTE_FINGERPRINT;
            rule.fingerprint = e->fingerprint;
        } else {
            rule.kind = OBSERVORE_MUTE_MAC;
            memcpy(rule.mac, e->mac, OBSERVORE_MAC_LEN);
        }

        size_t before = observore_mute_count();
        esp_err_t err = observore_mute_add(&rule);
        if (err == ESP_ERR_NO_MEM) {
            full++;
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        if (observore_mute_count() == before) {
            existing++;
            continue;
        }
        added++;
        switch (rule.kind) {
            case OBSERVORE_MUTE_NAME:        by_name++; break;
            case OBSERVORE_MUTE_FINGERPRINT: by_fp++;   break;
            default:
                by_mac++;
                if (e->addr_random) {
                    temporary++;   /* this one will be back under a new MAC */
                }
                break;
        }
    }

    /* Everything in range is now known, so the score and the log start from
     * a clean slate -- that is what makes it a baseline rather than just a
     * bulk mute. */
    observore_track_clear();

    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"seen\":%zu,\"added\":%zu,\"already\":%zu,"
             "\"by_name\":%zu,\"by_fingerprint\":%zu,\"by_mac\":%zu,"
             "\"temporary\":%zu,\"no_room\":%zu,\"rules\":%zu}",
             count, added, existing, by_name, by_fp, by_mac, temporary, full,
             observore_mute_count());
    ESP_LOGI(TAG, "baseline: %zu seen, %zu muted (%zu by name, %zu by "
                  "fingerprint, %zu by MAC of which %zu temporary), %zu no room",
             count, added, by_name, by_fp, by_mac, temporary, full);
    return send_json(req, body);
}

static esp_err_t notify_get_handler(httpd_req_t *req)
{
    char url[OBSERVORE_NOTIFY_URL_LEN] = {0};
    bool set = observore_notify_url(url, sizeof(url));
    char escaped[OBSERVORE_NOTIFY_URL_LEN * 2];
    json_escape(url, escaped, sizeof(escaped));
    char err[144];
    json_escape(observore_notify_last_error(), err, sizeof(err));

    char body[OBSERVORE_NOTIFY_URL_LEN * 2 + sizeof(err) + 256];
    /* The token is absent by design, exactly like the Wi-Fi password. */
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"url\":\"%s\",\"sent\":%" PRIu32 ","
             "\"failed\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"queued\":%zu,"
             "\"can_send\":%s,\"error\":\"%s\"}",
             set ? "true" : "false", escaped, observore_notify_sent(),
             observore_notify_failed(), observore_notify_dropped(),
             observore_notify_pending(),
             observore_wifi_mode() == OBSERVORE_MODE_UPLINK ? "true" : "false", err);
    return send_json(req, body);
}

static esp_err_t notify_set_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "clear", value, sizeof(value))) {
        observore_notify_clear();
        return send_json(req, "{\"ok\":true}");
    }
    if (query_param(req, "test", value, sizeof(value))) {
        if (observore_wifi_mode() != OBSERVORE_MODE_UPLINK) {
            return fail(req, "a test needs the uplink -- the SoftAP has no "
                             "route to your server");
        }
        esp_err_t err = observore_notify_test();
        if (err == ESP_ERR_INVALID_STATE) {
            return fail(req, "no server configured");
        }
        if (err != ESP_OK) {
            return fail(req, observore_notify_last_error());
        }
        return send_json(req, "{\"ok\":true}");
    }

    char url[OBSERVORE_NOTIFY_URL_LEN] = {0};
    char token[OBSERVORE_NOTIFY_TOKEN_LEN] = {0};
    if (!query_param(req, "url", url, sizeof(url))) {
        return fail(req, "url is required");
    }
    query_param(req, "token", token, sizeof(token));

    esp_err_t err = observore_notify_set(url, token);
    memset(token, 0, sizeof(token));
    if (err == ESP_ERR_INVALID_ARG) {
        return fail(req, "url must start with http:// or https://");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return fail(req, "url or token is too long");
    }
    if (err != ESP_OK) {
        return fail(req, "could not store the notifier settings");
    }
    ESP_LOGI(TAG, "notifier set to %s", url);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    observore_track_clear();
    ESP_LOGI(TAG, "log cleared by console");
    return send_json(req, "{\"ok\":true}");
}

esp_err_t observore_web_start(void)
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
        {.uri = "/api/notify",   .method = HTTP_GET,  .handler = notify_get_handler},
        {.uri = "/api/notify",   .method = HTTP_POST, .handler = notify_set_handler},
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

    /* Report the address that is actually reachable in this mode; naming the
     * SoftAP while joined to a network sends you to the wrong place. */
    if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
        ESP_LOGI(TAG, "console at http://%s/ (%s)", observore_wifi_uplink_ip(),
                 observore_wifi_hostname());
    } else {
        ESP_LOGI(TAG, "console at http://192.168.4.1/ (SSID %s)",
                 observore_wifi_ap_ssid());
    }
    return ESP_OK;
}

esp_err_t observore_web_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    scratch_free();
    return err;
}
