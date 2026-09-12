#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_auth.h"
#include "observore_clock.h"
#include "observore_notify.h"
#include "observore_util.h"
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

/* Scratch space for building responses.
 *
 * It MUST NOT be static internal RAM. It was originally two static snapshots
 * plus two static JSON buffers -- 84 KB of a part that has about 180 KB of
 * DRAM, on top of the 20 KB device table. Wi-Fi and lwip allocate from that
 * same pool, so under a few rounds of traffic free internal heap fell to
 * 1.4 KB with a largest free block of 768 bytes, and the SoftAP could still
 * beacon but could no longer allocate a buffer to answer an ARP request. The
 * console loaded once after boot and then went dead, looking for all the world
 * like a network fault.
 *
 * PSRAM is therefore the preferred home. But only some targets have any: the
 * C3, C5 and C6 have none, and demanding it there meant the firmware ran with
 * the console silently refusing to start. So a smaller budget is taken from
 * internal memory when there is no PSRAM, and the console reports fewer
 * devices per request rather than not existing.
 *
 * The buffers are shared between handlers, which is safe because
 * esp_http_server dispatches requests from a single task. */
#define JSON_BUF_PSRAM    (32 * 1024)
#define JSON_BUF_INTERNAL (8 * 1024)
/* Enough for the nearby list in full, and a useful slice of the device list. */
#define SNAP_INTERNAL     48

static observore_event_t *s_snap;
static size_t             s_snap_cap;
static char              *s_body;
static size_t             s_body_cap;

static bool scratch_alloc(void)
{
    if (s_snap && s_body) {
        return true;
    }

    s_snap = heap_caps_malloc(sizeof(observore_event_t) * OBSERVORE_MAX_DEVICES,
                              MALLOC_CAP_SPIRAM);
    s_body = heap_caps_malloc(JSON_BUF_PSRAM, MALLOC_CAP_SPIRAM);
    if (s_snap && s_body) {
        s_snap_cap = OBSERVORE_MAX_DEVICES;
        s_body_cap = JSON_BUF_PSRAM;
        return true;
    }

    /* No PSRAM, or not enough of it. Fall back to a deliberately smaller
     * budget in internal memory -- large enough to be useful, small enough
     * not to repeat the starvation described above. */
    free(s_snap);
    free(s_body);
    s_snap = malloc(sizeof(observore_event_t) * SNAP_INTERNAL);
    s_body = malloc(JSON_BUF_INTERNAL);
    if (!s_snap || !s_body) {
        ESP_LOGE(TAG, "no room for console scratch in PSRAM or internal RAM");
        free(s_snap);
        free(s_body);
        s_snap = NULL;
        s_body = NULL;
        return false;
    }
    s_snap_cap = SNAP_INTERNAL;
    s_body_cap = JSON_BUF_INTERNAL;
    ESP_LOGW(TAG, "no PSRAM: console scratch is %d KB of internal RAM and "
                  "reports at most %d devices per request",
             (int)((sizeof(observore_event_t) * SNAP_INTERNAL +
                    JSON_BUF_INTERNAL) / 1024), SNAP_INTERNAL);
    return true;
}

static void scratch_free(void)
{
    free(s_snap);
    free(s_body);
    s_snap = NULL;
    s_body = NULL;
    s_snap_cap = 0;
    s_body_cap = 0;
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

    /* Empty until the clock has been set. Reported rather than faked so the
     * console can say "times are relative to boot" instead of rendering an
     * uptime as though it were a date. */
    char now_iso[24];
    observore_clock_iso(now, now_iso, sizeof(now_iso));

    char body[768];
    observore_jbuf_t jb;
    observore_jb_init(&jb, body, sizeof(body), 2);   /* room for "}}" */

    observore_jb_printf(&jb,
        "{\"score\":%u,\"level\":\"%s\",\"devices\":%u,"
        "\"sightings\":%" PRIu32 ",\"uptime_s\":%" PRId64
        ",\"mode\":\"%s\",\"muted\":%zu,\"suppressed\":%" PRIu32
        ",\"time_valid\":%s,\"now\":\"%s\""
        ",\"counts\":{",
        st.score, observore_level_name(st.level), st.device_count,
        st.total_sightings, now / 1000000,
        observore_mode_name(observore_wifi_mode()),
        observore_mute_count(), observore_mute_suppressed(),
        observore_clock_valid() ? "true" : "false", now_iso);

    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        observore_jb_printf(&jb, "%s\"%s\":%" PRIu32, c > 1 ? "," : "",
                            observore_class_name(c), st.class_counts[c]);
    }
    observore_jb_close(&jb, "}}");
    return send_json(req, body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    observore_event_t *snap = s_snap;
    int64_t now = esp_timer_get_time();
    size_t count = observore_track_snapshot(snap, s_snap_cap);

    observore_jbuf_t jb;
    observore_jb_init(&jb, s_body, s_body_cap, 2);
    observore_jb_printf(&jb, "{\"devices\":[");

    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        const observore_event_t *e = &snap[i];
        char macbuf[OBSERVORE_MAC_STR_LEN];

        /* Empty when the clock has never been set, which the client reads as
         * "relative only" rather than being handed a timestamp from 1970. */
        char first_iso[24], last_iso[24];
        observore_clock_iso(e->first_seen_us, first_iso, sizeof(first_iso));
        observore_clock_iso(e->last_seen_us, last_iso, sizeof(last_iso));

        observore_jb_printf(&jb, "%s{\"mac\":\"%s\",\"class\":\"%s\",\"label\":\"",
                            i ? "," : "", observore_mac_str(e->mac, macbuf),
                            observore_class_name(e->cls));
        observore_jb_escape(&jb, e->label);
        observore_jb_printf(&jb, "\",\"vendor\":\"%s\",\"random\":%s,\"detail\":\"",
                            e->vendor ? e->vendor : "",
                            e->addr_random ? "true" : "false");
        observore_jb_escape(&jb, e->detail);
        observore_jb_printf(&jb,
            "\",\"evidence\":\"%s\",\"source\":\"%s\",\"rssi\":%d,"
            "\"channel\":%u,\"hits\":%" PRIu32 ",\"first_seen_s\":%" PRId64
            ",\"last_seen_s\":%" PRId64
            ",\"first_seen\":\"%s\",\"last_seen\":\"%s\"}",
            observore_evidence_name(e->evidence), observore_source_name(e->src),
            e->rssi, e->channel, e->hits,
            (now - e->first_seen_us) / 1000000,
            (now - e->last_seen_us) / 1000000,
            first_iso, last_iso);

        if (observore_jb_full(&jb)) {
            ESP_LOGW(TAG, "device list truncated at %zu of %zu", written, count);
            break;
        }
        written++;
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, s_body);
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

static esp_err_t ok(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t fail(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    char body[160];
    char safe[96];
    observore_json_escape(why, safe, sizeof(safe));
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", safe);
    return send_json(req, body);
}

static esp_err_t mutes_handler(httpd_req_t *req)
{
    /* Walked by index into the PSRAM scratch.  A static copy of the whole rule
     * table was 6 KB of internal RAM duplicating observore_mute's own, resident
     * even while the server is stopped. */
    size_t n = observore_mute_count();

    observore_jbuf_t jb;
    observore_jb_init(&jb, s_body, s_body_cap, 2);
    observore_jb_printf(&jb, "{\"suppressed\":%" PRIu32 ",\"rules\":[",
                        observore_mute_suppressed());

    for (size_t i = 0; i < n; i++) {
        observore_mute_rule_t r;
        if (!observore_mute_get(i, &r)) {
            break;   /* the list shrank under us */
        }
        observore_jb_printf(&jb, "%s{\"index\":%zu,\"kind\":\"%s\",\"value\":\"",
                            i ? "," : "", i, observore_mute_kind_name(r.kind));
        switch (r.kind) {
            case OBSERVORE_MUTE_MAC: {
                char macbuf[OBSERVORE_MAC_STR_LEN];
                observore_jb_printf(&jb, "%s", observore_mac_str(r.mac, macbuf));
                break;
            }
            case OBSERVORE_MUTE_OUI:
                observore_jb_printf(&jb, "%02X:%02X:%02X", r.mac[0], r.mac[1],
                                    r.mac[2]);
                break;
            case OBSERVORE_MUTE_CLASS:
                observore_jb_printf(&jb, "%s", observore_class_name(r.cls));
                break;
            case OBSERVORE_MUTE_FINGERPRINT:
                observore_jb_printf(&jb, "%08" PRIx32, r.fingerprint);
                break;
            case OBSERVORE_MUTE_NAME:
                observore_jb_escape(&jb, r.ssid);
                break;
            default:
                /* A kind added without extending this switch should be
                 * visible, not silently blank. */
                observore_jb_printf(&jb, "unrenderable kind %u", r.kind);
                break;
        }
        observore_jb_printf(&jb, "\"}");
        if (observore_jb_full(&jb)) {
            ESP_LOGW(TAG, "mute list truncated at %zu of %zu", i, n);
            break;
        }
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, s_body);
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

    esp_err_t err = observore_mute_add(&rule, NULL);
    if (err == ESP_ERR_NO_MEM) {
        return fail(req, "mute list is full");
    }
    if (err != ESP_OK) {
        return fail(req, "invalid rule");
    }
    ESP_LOGI(TAG, "muted %s", observore_mute_kind_name(rule.kind));
    return ok(req);
}

static esp_err_t unmute_handler(httpd_req_t *req)
{
    char value[16];
    if (query_param(req, "all", value, sizeof(value))) {
        observore_mute_clear();
        return ok(req);
    }
    if (!query_param(req, "index", value, sizeof(value))) {
        return fail(req, "expected index or all=1");
    }
    if (observore_mute_remove((size_t)strtoul(value, NULL, 10)) != ESP_OK) {
        return fail(req, "no such rule");
    }
    return ok(req);
}

static esp_err_t netcfg_get_handler(httpd_req_t *req)
{
    char ssid[OBSERVORE_SSID_LEN] = {0};
    bool set = observore_netcfg_ssid(ssid, sizeof(ssid));
    char escaped[OBSERVORE_SSID_LEN * 2];
    observore_json_escape(ssid, escaped, sizeof(escaped));

    char werr[224];
    observore_json_escape(observore_wifi_uplink_error(), werr, sizeof(werr));

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
        return ok(req);
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
    return ok(req);
}

/* Unclassified devices, so known gear can be recognised and muted before it
 * ever trips the follower heuristic. */
#define NEARBY_MAX 40

static esp_err_t nearby_handler(httpd_req_t *req)
{
    observore_event_t *snap = s_snap;
    int64_t now = esp_timer_get_time();
    size_t count = observore_track_nearby(snap,
                                          s_snap_cap < NEARBY_MAX
                                              ? s_snap_cap : NEARBY_MAX);

    observore_jbuf_t jb;
    observore_jb_init(&jb, s_body, s_body_cap, 2);
    observore_jb_printf(&jb, "{\"nearby\":[");

    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        const observore_event_t *e = &snap[i];
        char macbuf[OBSERVORE_MAC_STR_LEN];
        char last_iso[24];
        observore_clock_iso(e->last_seen_us, last_iso, sizeof(last_iso));

        observore_jb_printf(&jb, "%s{\"mac\":\"%s\",\"vendor\":\"%s\",\"name\":\"",
                            i ? "," : "", observore_mac_str(e->mac, macbuf),
                            e->vendor ? e->vendor : "");
        /* The name is chosen by whoever owns the radio, so it is escaped on
         * the way out exactly like every other remote-controlled string. */
        observore_jb_escape(&jb, e->detail);
        observore_jb_printf(&jb,
            "\",\"random\":%s,\"source\":\"%s\",\"fingerprint\":\"%08" PRIx32 "\","
            "\"rssi\":%d,\"hits\":%" PRIu32 ",\"last_seen_s\":%" PRId64
            ",\"last_seen\":\"%s\"}",
            e->addr_random ? "true" : "false", observore_source_name(e->src),
            e->fingerprint, e->rssi, e->hits,
            (now - e->last_seen_us) / 1000000, last_iso);

        if (observore_jb_full(&jb)) {
            ESP_LOGW(TAG, "nearby list truncated at %zu of %zu", written, count);
            break;
        }
        written++;
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, s_body);
}

/* Mark everything currently in range as known, then start watching for what
 * changes from here.  Both classified and unclassified devices are muted: at
 * home your own doorbell camera is exactly the thing you want silenced, and
 * the action is fully reversible from the same panel. */
static esp_err_t baseline_handler(httpd_req_t *req)
{
    observore_event_t *snap = s_snap;
    size_t count = observore_track_all(snap, s_snap_cap);

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

        bool is_new = false;
        esp_err_t err = observore_mute_add_deferred(&rule, &is_new);
        if (err == ESP_ERR_NO_MEM) {
            full++;
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        if (!is_new) {
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

    /* One flash write for the whole baseline rather than one per rule. */
    observore_mute_save();

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
    observore_json_escape(url, escaped, sizeof(escaped));
    char err[144];
    observore_json_escape(observore_notify_last_error(), err, sizeof(err));

    char body[OBSERVORE_NOTIFY_URL_LEN * 2 + sizeof(err) + 256];
    /* The token is absent by design, exactly like the Wi-Fi password. */
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"url\":\"%s\",\"provider\":\"%s\","
             "\"needs_user\":%s,\"has_user\":%s,\"url_hint\":\"%s\","
             "\"sent\":%" PRIu32 ","
             "\"failed\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"queued\":%zu,"
             "\"can_send\":%s,\"error\":\"%s\"}",
             set ? "true" : "false", escaped,
             observore_provider_name(observore_notify_provider()),
             observore_provider_needs_user(observore_notify_provider())
                 ? "true" : "false",
             observore_notify_has_user() ? "true" : "false",
             observore_provider_url_hint(observore_notify_provider()),
             observore_notify_sent(),
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
        return ok(req);
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
        return ok(req);
    }

    char url[OBSERVORE_NOTIFY_URL_LEN] = {0};
    char token[OBSERVORE_NOTIFY_TOKEN_LEN] = {0};
    char user[OBSERVORE_NOTIFY_USER_LEN] = {0};
    char provname[16] = {0};

    observore_provider_t provider = OBSERVORE_PROVIDER_GOTIFY;
    if (query_param(req, "provider", provname, sizeof(provname))) {
        if (!observore_provider_from_name(provname, &provider)) {
            return fail(req, "unknown provider -- expected gotify, ntfy or "
                             "pushover");
        }
    } else {
        provider = observore_notify_provider();
    }
    query_param(req, "url", url, sizeof(url));
    query_param(req, "token", token, sizeof(token));
    query_param(req, "user", user, sizeof(user));

    esp_err_t err = observore_notify_set(provider, url, token, user);
    memset(token, 0, sizeof(token));
    memset(user, 0, sizeof(user));
    if (err == ESP_ERR_INVALID_ARG) {
        if (observore_provider_needs_user(provider) && !*user) {
            return fail(req, "this provider needs a user key as well as a token");
        }
        return fail(req, "a url is required, starting with http:// or https://");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return fail(req, "url or token is too long");
    }
    if (err != ESP_OK) {
        return fail(req, "could not store the notifier settings");
    }
    ESP_LOGI(TAG, "notifier set to %s via %s", url,
             observore_provider_name(provider));
    return ok(req);
}

static esp_err_t unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return send_json(req, "{\"ok\":false,\"error\":\"authentication required\"}");
}

static esp_err_t login_handler(httpd_req_t *req)
{
    char password[OBSERVORE_PASSWORD_LEN] = {0};
    if (!query_param(req, "password", password, sizeof(password))) {
        return fail(req, "password is required");
    }
    esp_err_t err = observore_auth_login(req, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        /* Deliberately says nothing about which part was wrong. */
        httpd_resp_set_status(req, "401 Unauthorized");
        return send_json(req, "{\"ok\":false,\"error\":\"wrong password\"}");
    }
    return ok(req);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    observore_auth_logout(req);
    return ok(req);
}

/* Reports whether a login is needed, so the console can show the form instead
 * of guessing from a failed fetch. Open by design: it reveals nothing. */
static esp_err_t authstate_handler(httpd_req_t *req)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"required\":%s,\"authenticated\":%s}",
             observore_auth_enforced() ? "true" : "false",
             observore_auth_ok(req) ? "true" : "false");
    return send_json(req, body);
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    observore_track_clear();
    ESP_LOGI(TAG, "log cleared by console");
    return ok(req);
}

typedef struct {
    const char     *uri;
    httpd_method_t  method;
    esp_err_t     (*fn)(httpd_req_t *);
    bool            open_route;   /* reachable without a session */
} observore_route_t;

static esp_err_t dispatch(httpd_req_t *req)
{
    const observore_route_t *r = req->user_ctx;
    if (!r->open_route && !observore_auth_ok(req)) {
        return unauthorized(req);
    }
    return r->fn(req);
}

esp_err_t observore_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    /* The flag lives in the table and one dispatcher enforces it, so a route
     * added later cannot quietly forget to check. Only the page itself and the
     * two login endpoints are open -- the page carries no data, and the rest
     * is the detection log and the device's own configuration. */
    static observore_route_t routes[] = {
        {"/",              HTTP_GET,  index_handler,      true },
        {"/api/auth",      HTTP_GET,  authstate_handler,  true },
        {"/api/login",     HTTP_POST, login_handler,      true },
        {"/api/logout",    HTTP_POST, logout_handler,     true },
        {"/api/status",    HTTP_GET,  status_handler,     false},
        {"/api/devices",   HTTP_GET,  devices_handler,    false},
        {"/api/nearby",    HTTP_GET,  nearby_handler,     false},
        {"/api/clear",     HTTP_POST, clear_handler,      false},
        {"/api/mutes",     HTTP_GET,  mutes_handler,      false},
        {"/api/mute",      HTTP_POST, mute_handler,       false},
        {"/api/unmute",    HTTP_POST, unmute_handler,     false},
        {"/api/baseline",  HTTP_POST, baseline_handler,   false},
        {"/api/netcfg",    HTTP_GET,  netcfg_get_handler, false},
        {"/api/netcfg",    HTTP_POST, netcfg_set_handler, false},
        {"/api/notify",    HTTP_GET,  notify_get_handler, false},
        {"/api/notify",    HTTP_POST, notify_set_handler, false},
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
        const httpd_uri_t u = {
            .uri      = routes[i].uri,
            .method   = routes[i].method,
            .handler  = dispatch,
            .user_ctx = &routes[i],
        };
        err = httpd_register_uri_handler(s_server, &u);
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
