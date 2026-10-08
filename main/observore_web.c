#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "observore_battery.h"
#include "observore_census.h"
#include "observore_monitors.h"
#include "observore_peer.h"
#include "observore_motion.h"
#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_auth.h"
#include "observore_clock.h"
#include "observore_history.h"
#include "observore_notify.h"
#include "observore_util.h"
#include "observore_track.h"
#include "observore_update.h"
#include "observore_display.h"
#include "observore_heapwatch.h"
#include "observore_runs.h"
#include "observore_web.h"
#include "observore_wifi.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
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

extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_gz_end");

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
/* Half of what this was, because on a board with no PSRAM it is held for the
 * whole uplink window and competes with the TLS handshake behind the update
 * check -- which on the CYD it beat, leaving the device unable to see a new
 * release at all. The console pages itself anyway; a shorter list per request
 * is a smaller cost than a device that cannot update. */
#define JSON_BUF_INTERNAL (4 * 1024)
/* Enough for the nearby list in full, and a useful slice of the device list. */
#define SNAP_INTERNAL     24

static observore_event_t *s_snap;
static size_t             s_snap_cap;
static char              *s_body;
static size_t             s_body_cap;

/* The body buffer, taken on its own.
 *
 * It used to be one decision with the snapshot: both or neither. /api/status
 * needs only the body, and on the 3.5" CYD the pair does not fit while the
 * body alone does -- so the board least able to report on itself was the one
 * that could not serve its own status, which is also the one that has been
 * rebooting. The instrument has to work before the fault can be read.
 *
 * The internal size steps down to what the heap can actually give. With the
 * caps corrected (see observore_heapwatch.h) that board's largest free block
 * is about 4.6 KB and its low-water mark has been under 1.4 KB, so a fixed
 * 4 KB request is a request it cannot always meet. */
static const size_t BODY_STEPS[] = {JSON_BUF_INTERNAL, 3 * 1024, 2 * 1024};

static bool body_alloc(void)
{
    if (s_body) {
        return true;
    }
    s_body = heap_caps_malloc(JSON_BUF_PSRAM, MALLOC_CAP_SPIRAM);
    if (s_body) {
        s_body_cap = JSON_BUF_PSRAM;
        return true;
    }
    for (size_t i = 0; i < sizeof(BODY_STEPS) / sizeof(BODY_STEPS[0]); i++) {
        s_body = malloc(BODY_STEPS[i]);
        if (s_body) {
            s_body_cap = BODY_STEPS[i];
            return true;
        }
    }
    s_body_cap = 0;
    return false;
}

/* The device-table snapshot, likewise. Only the three handlers that walk the
 * table need it, and on a tight board it is the larger half of the pair. */
static bool snap_alloc(void)
{
    if (s_snap) {
        return true;
    }
    s_snap = heap_caps_malloc(sizeof(observore_event_t) * OBSERVORE_MAX_DEVICES,
                              MALLOC_CAP_SPIRAM);
    if (s_snap) {
        s_snap_cap = OBSERVORE_MAX_DEVICES;
        return true;
    }
    s_snap = malloc(sizeof(observore_event_t) * SNAP_INTERNAL);
    if (s_snap) {
        s_snap_cap = SNAP_INTERNAL;
        return true;
    }
    s_snap_cap = 0;
    return false;
}

/* Said once, not once per uplink window: a warning repeated every few minutes
 * for the life of the device -- 536 times in one night on a board that has
 * never had PSRAM -- is not a warning any more, just the log getting harder
 * to read. */
static void say_budget_once(void)
{
    static bool s_said;
    if (s_said || s_body_cap == 0 || s_body_cap == JSON_BUF_PSRAM) {
        return;
    }
    s_said = true;
    /* The body only. The snapshot is a separate allocation now and may not
     * have been attempted yet, so naming its capacity here reported "at most
     * 0 devices" on a board whose snapshot was simply not asked for -- a
     * number about nothing, which is the kind this project has just spent a
     * day removing. */
    ESP_LOGW(TAG, "no PSRAM: console body buffer is %u bytes of internal RAM",
             (unsigned)s_body_cap);
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
    /* Served compressed unconditionally rather than sniffing Accept-Encoding,
     * because holding an uncompressed copy as well would spend the flash this
     * saves. Every browser has accepted gzip for two decades; a command-line
     * client needs --compressed or equivalent, which the README says.
     *
     * No -1 on the length here: that belonged to the NUL the text embed added,
     * and a BINARY embed has none. Taking a byte off a gzip stream truncates
     * the CRC and the browser rejects the whole page. */
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* The console's scratch, or NULL with a 503 already sent.
 *
 * Asked for by the handlers that need a buffer, rather than taken for every
 * request by the dispatcher. That arrangement cost the one thing the console
 * is for on a board that has just booted: at 3.5 seconds the device announces
 * `console at http://...` while Wi-Fi and BLE are still starting, internal
 * heap is at twelve kilobytes with a ten-kilobyte largest block, and the
 * scratch does not fit. /api/login was then refused with 503 -- a login that
 * writes `{"ok":true}` and needs no buffer at all.
 *
 * That is the worst possible moment to be locked out, because a board with no
 * RTC chip is undated for those first seconds and setting the clock by hand
 * is exactly what somebody would be logging in to do.
 *
 * A helper rather than a flag in the route table, so the rule is kept by
 * construction: a handler that uses the buffer has to obtain it here, and one
 * that does not never asks. A flag can be forgotten by the next route, and
 * forgetting it would mean writing into a null pointer. */
static esp_err_t no_room(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return send_json(req,
        "{\"ok\":false,\"error\":\"not enough memory right now\"}");
}

static char *scratch_body(httpd_req_t *req)
{
    if (body_alloc()) {
        say_budget_once();
        return s_body;
    }
    no_room(req);
    return NULL;
}

/* The snapshot, for the three handlers that walk the device table. Separate
 * from the body so a handler asks for what it uses and no more. */
static observore_event_t *scratch_snap(httpd_req_t *req)
{
    if (snap_alloc()) {
        return s_snap;
    }
    no_room(req);
    return NULL;
}

/* The one write that cannot be allowed to be dropped.
 *
 * The buffer drops a write whole rather than truncating it, which keeps a
 * *list* parseable: the elements that fitted, then the closing bracket from
 * the reserve. It does not keep a single large object parseable. /api/status
 * is one printf, so a body too small loses the whole thing, and the reserve
 * then closes a document that never opened -- the response is `}}`.
 *
 * observore_jb_full() has existed all along and nothing read it, so this was
 * waiting to happen the first time the buffer got smaller. Checked right
 * after a document's opening write, where it means there is no document. */
static bool opened(observore_jbuf_t *jb)
{
    return !observore_jb_full(jb);
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
    const observore_heap_event_t *latest = observore_heapwatch_latest();

    /* What other nodes have warned about. A receive-only node emits nothing
     * and still benefits from every warning in range, so this is the number
     * that says whether that is working. */
    int peer_nodes = 0, peer_warnings = 0;
    observore_peer_counts(now, &peer_nodes, &peer_warnings);

    /* What the census has learned, and what it is doing about it. "known"
     * climbing while "household" stays at nothing would mean the rule is
     * never being satisfied, and there is no other way to see that from
     * outside. `quieted` and `dampened` are the live half: how many of the
     * devices in front of the radio right now are having weight taken off
     * the score, which is the only thing that explains the number. */
    int census_known = 0, census_household = 0;
    observore_census_counts(observore_clock_day(),
                            &census_household, &census_known);

    /* The last check's error, escaped: it can carry an esp-tls string. */
    char cerr[96];
    observore_json_escape(observore_update_error(), cerr, sizeof(cerr));

    /* The shared scratch rather than the stack: the status grew past what a
     * handler's stack frame should carry once it started listing runs, and
     * esp_http_server serves one request at a time, so nothing else is in it. */
    char *body = scratch_body(req);
    if (!body) {
        return ESP_OK;
    }
    observore_jbuf_t jb;
    observore_jb_init(&jb, body, s_body_cap, 2);   /* room for "}}" */

    observore_jb_printf(&jb,
        "{\"score\":%u,\"level\":\"%s\",\"devices\":%u,"
        "\"sightings\":%" PRIu32 ",\"uptime_s\":%" PRId64
        ",\"mode\":\"%s\",\"muted\":%zu,\"suppressed\":%" PRIu32
        ",\"time_valid\":%s,\"now\":\"%s\",\"clock\":\"%s\""
        ",\"version\":\"%s\",\"board\":\"%s\""
        ",\"latest\":\"%s\",\"update\":%s"
        ",\"update_state\":\"%s\",\"update_pct\":%d"
        ",\"checked_s\":%ld,\"checking\":%s,\"check_error\":\"%s\""
        ",\"heap\":{\"free\":%u,\"min\":%u,\"largest\":%u"
        ",\"min_at_s\":%lld,\"min_mode\":\"%s\",\"min_queued\":%u}"
        ",\"reset_reason\":\"%s\",\"notifier\":%s"
        ",\"bright\":%d,\"bright_now\":%d,\"bright_steps\":%d"
        ",\"motion\":{\"sensor\":%s,\"carried\":%s,\"journeys\":%u"
        ",\"last_overlap_pct\":%d,\"last_faded_db\":%d}"
        ",\"battery\":{\"sense\":%s,\"mv\":%d,\"pct\":%d}"
        ",\"taps\":%u"
        ",\"monitors\":{\"off\":%d,\"off_mask\":%lu}"
        ",\"peers\":{\"nodes\":%d,\"warnings\":%d}"
        ",\"census\":{\"known\":%d,\"household\":%d,\"days\":%d"
        ",\"quieted\":%d,\"dampened\":%d}"
        ",\"counts\":{",
        st.score, observore_level_name(st.level), st.device_count,
        st.total_sightings, now / 1000000,
        observore_mode_name(observore_wifi_mode()),
        observore_mute_count(), observore_mute_suppressed(),
        observore_clock_valid() ? "true" : "false", now_iso,
        observore_clock_source_name(observore_clock_source()),
        observore_update_running_version(),
        CONFIG_OBSERVORE_BOARD,
        observore_update_latest_version(),
        observore_update_available() ? "true" : "false",
        observore_update_state_name(observore_update_state()),
        observore_update_progress(),
        observore_update_age_s(),
        observore_update_check_pending() ? "true" : "false",
        cerr,
        (unsigned)heap_caps_get_free_size(OBSERVORE_HEAP_CAPS),
        (unsigned)heap_caps_get_minimum_free_size(OBSERVORE_HEAP_CAPS),
        (unsigned)heap_caps_get_largest_free_block(OBSERVORE_HEAP_CAPS),
        latest ? (long long)(latest->at_us / 1000000) : -1LL,
        latest ? latest->mode : "",
        latest ? (unsigned)latest->queued : 0u,
        observore_reset_reason_name(esp_reset_reason()),
#if CONFIG_OBSERVORE_NOTIFIER
        "true"
#else
        "false"
#endif
        , observore_display_brightness(),
        observore_display_brightness_effective(),
        observore_display_has_light_sensor() ? OBSERVORE_BRIGHT_STEPS + 1
                                            : OBSERVORE_BRIGHT_STEPS,
        observore_motion_available() ? "true" : "false",
        observore_motion_moving() ? "true" : "false",
        (unsigned)observore_motion_journeys(),
        observore_motion_last_overlap_pct(),
        observore_motion_last_faded_db(),
        observore_battery_available() ? "true" : "false",
        observore_battery_mv(),
        observore_battery_pct_from_mv(observore_battery_mv()),
        (unsigned)observore_display_taps(),
        st.monitors_off, (unsigned long)observore_monitors_off_mask(),
        peer_nodes, peer_warnings,
        census_known, census_household, OBSERVORE_CENSUS_MIN_DAYS,
        st.census_quieted, st.census_dampened);

    /* Everything above is one write, so if it did not fit there is nothing
     * to send: the reserve would close a document that never opened and the
     * reply would be `}}`. An error is the honest answer, and the body
     * stepping down to what a tight board can give is what makes this
     * reachable rather than theoretical. */
    if (!opened(&jb)) {
        ESP_LOGW(TAG, "the status did not fit a %u-byte body",
                 (unsigned)s_body_cap);
        return no_room(req);
    }

    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        observore_jb_printf(&jb, "%s\"%s\":%" PRIu32, c > 1 ? "," : "",
                            observore_class_name(c), st.class_counts[c]);
    }

    /* Completed runs, newest first: how long each lasted and how it ended.
     * On battery, a run that ended in a brownout or a power-on is the
     * battery's actual life, measured rather than guessed. */
    observore_run_t runs[OBSERVORE_RUNS_MAX];
    size_t nruns = observore_runs_list(runs, OBSERVORE_RUNS_MAX);
    observore_jb_printf(&jb, "},\"runs\":[");
    for (size_t i = 0; i < nruns; i++) {
        /* The voltages are reported as zero when the board cannot measure
         * its own supply, and the console says nothing rather than drawing a
         * flat cell. Only the round AMOLED board has the sense pin; the CYDs
         * and the devkit have theirs taken by the display and touch. */
        observore_jb_printf(&jb,
            "%s{\"up_s\":%" PRIu32 ",\"end\":\"%s\""
            ",\"mv_start\":%u,\"mv_end\":%u}",
            i ? "," : "", runs[i].up_s,
            observore_reset_reason_name((esp_reset_reason_t)runs[i].end),
            (unsigned)runs[i].mv_start, (unsigned)runs[i].mv_end);
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    if (!scratch_body(req) || !scratch_snap(req)) {
        return ESP_OK;
    }
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
            "\"channel\":%u,\"hits\":%" PRIu32 ",\"rotations\":%u,\"first_seen_s\":%" PRId64
            ",\"last_seen_s\":%" PRId64
            ",\"first_seen\":\"%s\",\"last_seen\":\"%s\"}",
            observore_evidence_name(e->evidence), observore_source_name(e->src),
            e->rssi, e->channel, e->hits, (unsigned)e->rotations,
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
    if (!scratch_body(req)) {
        return ESP_OK;
    }
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
        /* What the rule has actually done, not just what it says. A rule
         * silencing a whole population reads exactly like a quiet room from
         * outside; this is what makes the difference visible. */
        observore_mute_stat_t st = {0};
        observore_mute_stat(i, &st);
        if (st.suppressed == 0 && !st.disabled) {
            /* Said only of the rules it is true of. Most rules have never
             * fired, and on a board with no PSRAM the whole list has to fit in
             * a four-kilobyte buffer -- spending thirty bytes a rule on three
             * zeroes truncated the list instead. */
            observore_jb_printf(&jb, "\"}");
        } else {
            observore_jb_printf(&jb, "\",\"suppressed\":%" PRIu32
                                     ",\"addresses\":%u,\"disabled\":%s}",
                                st.suppressed, (unsigned)st.addresses,
                                st.disabled ? "true" : "false");
        }
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
    if (err == ESP_OK) {
        /* So the list the console is about to redraw agrees with the rule it
         * was just given. */
        observore_track_forget_muted();
    }
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

/* Discarding history is a POST, not a GET with a parameter.  A browser will
 * prefetch, preload and retry a GET on its own, and none of those should be
 * able to throw away the record. */
static esp_err_t history_clear_handler(httpd_req_t *req)
{
    observore_history_clear();
    return ok(req);
}

/* What the device saw before the last restart, and since.
 *
 * Deliberately separate from /api/devices: that reports the live table, which
 * is working state and empties on reboot. This is the part with lasting value,
 * and rows restored from flash are marked so the console does not present a
 * detection from three days ago as though it were happening now. */
static esp_err_t history_handler(httpd_req_t *req)
{
    if (!scratch_body(req)) {
        return ESP_OK;
    }
    static observore_history_entry_t rows[OBSERVORE_HISTORY_MAX];
    size_t count = observore_history_copy(rows, OBSERVORE_ARRLEN(rows));

    observore_jbuf_t jb;
    observore_jb_init(&jb, s_body, s_body_cap, 2);
    observore_jb_printf(&jb, "{\"history\":[");

    for (size_t i = 0; i < count; i++) {
        const observore_history_entry_t *e = &rows[i];
        char macbuf[OBSERVORE_MAC_STR_LEN];
        char first[24] = {0}, last[24] = {0};
        if (e->first_epoch) {
            struct tm t;
            time_t v = (time_t)e->first_epoch;
            gmtime_r(&v, &t);
            strftime(first, sizeof(first), "%Y-%m-%dT%H:%M:%SZ", &t);
        }
        if (e->last_epoch) {
            struct tm t;
            time_t v = (time_t)e->last_epoch;
            gmtime_r(&v, &t);
            strftime(last, sizeof(last), "%Y-%m-%dT%H:%M:%SZ", &t);
        }

        observore_jb_printf(&jb, "%s{\"mac\":\"%s\",\"class\":\"%s\",\"label\":\"",
                            i ? "," : "", observore_mac_str(e->mac, macbuf),
                            observore_class_name(e->cls));
        observore_jb_escape(&jb, e->label);
        observore_jb_printf(&jb,
            "\",\"rssi\":%d,\"hits\":%" PRIu32 ",\"first_seen\":\"%s\","
            "\"last_seen\":\"%s\",\"this_boot\":%s}",
            e->rssi, e->hits, first, last,
            e->last_us ? "true" : "false");

        if (observore_jb_full(&jb)) {
            break;
        }
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, s_body);
}

static esp_err_t nearby_handler(httpd_req_t *req)
{
    if (!scratch_body(req) || !scratch_snap(req)) {
        return ESP_OK;
    }
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
    /* The snapshot only: this one answers from a stack buffer, so asking for
     * the JSON body as well would be asking a tight board for memory the
     * reply never touches. */
    if (!scratch_snap(req)) {
        return ESP_OK;
    }
    observore_baseline_t b;
    observore_mute_baseline(s_snap, s_snap_cap, &b);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"seen\":%zu,\"added\":%zu,\"already\":%zu,"
             "\"by_name\":%zu,\"by_fingerprint\":%zu,\"by_mac\":%zu,"
             "\"temporary\":%zu,\"no_room\":%zu,\"protected\":%zu,"
             "\"protected_class\":\"%s\",\"rules\":%zu}",
             b.seen, b.added, b.already, b.by_name, b.by_fingerprint, b.by_mac,
             b.temporary, b.no_room, b.protected_muted,
             b.protected_muted ? observore_class_desc(
                 (observore_class_t)b.protected_example)->name : "",
             observore_mute_count());
    ESP_LOGI(TAG, "baseline: %zu seen, %zu muted (%zu by name, %zu by "
                  "fingerprint, %zu by MAC of which %zu temporary), %zu no "
                  "room, %zu of a protected class",
             b.seen, b.added, b.by_name, b.by_fingerprint, b.by_mac,
             b.temporary, b.no_room, b.protected_muted);
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
             "\"retry_in_s\":%" PRIu32 ","
             "\"sent\":%" PRIu32 ","
             "\"failed\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"queued\":%zu,"
             "\"can_send\":%s,\"error\":\"%s\"}",
             set ? "true" : "false", escaped,
             observore_provider_name(observore_notify_provider()),
             observore_provider_needs_user(observore_notify_provider())
                 ? "true" : "false",
             observore_notify_has_user() ? "true" : "false",
             observore_provider_url_hint(observore_notify_provider()),
             observore_notify_retry_in_s(),
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
            return fail(req, "unknown provider -- expected gotify, ntfy, "
                             "pushover, webhook or telegram");
        }
    } else {
        provider = observore_notify_provider();
    }
    query_param(req, "url", url, sizeof(url));
    query_param(req, "token", token, sizeof(token));
    query_param(req, "user", user, sizeof(user));

    /* Decided before the buffers are wiped, because they are wiped before the
     * error is reported and the old check ran on the empty buffer -- so it
     * could never be true and the message it guarded was never shown. */
    bool same_provider = (provider == observore_notify_provider());
    bool blank_token   = !*token;
    bool blank_user    = !*user;

    esp_err_t err = observore_notify_set(provider, url, token, user);
    memset(token, 0, sizeof(token));
    memset(user, 0, sizeof(user));
    if (err == ESP_ERR_INVALID_ARG) {
        if (observore_provider_needs_user(provider) && blank_user) {
            return fail(req, "this provider needs a second credential -- a "
                             "user key or chat id -- as well as a token");
        }
        return fail(req, "a url is required, starting with http:// or https://");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return fail(req, "url or token is too long");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return fail(req, "this build has no notifier");
    }
    if (err != ESP_OK) {
        return fail(req, "could not store the notifier settings");
    }
    /* Provider only. The URL can be the credential -- a webhook usually is --
     * and this line ends up in bug reports. */
    ESP_LOGI(TAG, "notifier set to %s", observore_provider_name(provider));

    /* Tell the page what happened to the credentials, so it can say "token
     * kept" or "token cleared" instead of leaving the user to find out at the
     * next send. */
    char body[96];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"kept_token\":%s,\"kept_user\":%s}",
             (same_provider && blank_token) ? "true" : "false",
             (same_provider && blank_user)  ? "true" : "false");
    return send_json(req, body);
}

/* Every recorded drop, oldest first. Small enough to build in one buffer:
 * eight events at under sixty characters each. */
static esp_err_t heap_handler(httpd_req_t *req)
{
    observore_heap_event_t ev[OBSERVORE_HEAPWATCH_EVENTS];
    size_t n = observore_heapwatch_events(ev, OBSERVORE_HEAPWATCH_EVENTS);

    char body[640];
    observore_jbuf_t jb;
    observore_jb_init(&jb, body, sizeof(body), 2);
    observore_jb_printf(&jb, "{\"events\":[");
    for (size_t i = 0; i < n; i++) {
        observore_jb_printf(&jb,
            "%s{\"at_s\":%lld,\"min\":%" PRIu32 ",\"largest\":%" PRIu32
            ",\"queued\":%" PRIu32 ",\"mode\":\"%s\"}",
            i ? "," : "", (long long)(ev[i].at_us / 1000000),
            ev[i].free_min, ev[i].largest, ev[i].queued, ev[i].mode);
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, body);
}

static esp_err_t census_handler(httpd_req_t *req)
{
    if (!scratch_body(req)) {
        return ESP_OK;
    }
    /* What the census knows, so that it can be judged before it is trusted.
     *
     * The address count is the number this exists to show. An identity seen
     * under many addresses is ambiguous -- one device rotating, or several
     * identical ones sharing an advert shape -- and nothing here can tell
     * those apart. How often that happens is what decides whether suppression
     * keyed on a fingerprint is viable at all, and it is invisible without
     * this. */
    const observore_census_entry_t *tab = NULL;
    size_t n = observore_census_entries(&tab);
    int day = observore_clock_day();

    char *body = s_body;
    observore_jbuf_t jb;
    observore_jb_init(&jb, body, s_body_cap, 2);
    observore_jb_printf(&jb,
        "{\"day\":%d,\"min_days\":%d,\"window\":%d,\"addr_limit\":%d"
        ",\"max\":%d,\"full\":%s,\"members\":[",
        day, OBSERVORE_CENSUS_MIN_DAYS, OBSERVORE_CENSUS_WINDOW,
        OBSERVORE_CENSUS_ADDRS, OBSERVORE_CENSUS_MAX,
        observore_census_full() ? "true" : "false");
    for (size_t i = 0; i < n; i++) {
        bool over = false;
        int addrs = observore_census_addresses(tab[i].id, &over);
        observore_jb_printf(&jb,
            "%s{\"id\":\"%08lx\",\"days\":%d,\"household\":%s"
            ",\"addresses\":%d,\"more\":%s,\"last_day\":%u}",
            i ? "," : "", (unsigned long)tab[i].id,
            observore_census_days_seen(tab[i].id, day),
            observore_census_is_household(tab[i].id, day) ? "true" : "false",
            addrs, over ? "true" : "false", (unsigned)tab[i].last_day);
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, body);
}

/* The time, from the browser asking.
 *
 * Most of these boards have no RTC chip, and the places this device is worth
 * carrying are routinely places where no NTP server is reachable -- a camera
 * VLAN with no route out, a field with no uplink at all. Those boards cold
 * boot into 1970 and stay there, which means findings with no date on them
 * and a census that cannot count a single day.
 *
 * The browser on the other end of this request knows the time to the
 * millisecond. It is a worse clock than SNTP and a far better one than none,
 * so it is accepted and recorded as what it is: set by a person. A later SNTP
 * reply replaces it without being asked.
 *
 * Behind the console password like everything else here. That is not about
 * the time being secret -- it is that a clock is now load-bearing for what
 * the census suppresses, and an unauthenticated endpoint that moves the date
 * is an unauthenticated endpoint that decides what the device stops
 * reporting. */
static esp_err_t time_handler(httpd_req_t *req)
{
    char value[24];
    if (!query_param(req, "epoch", value, sizeof(value))) {
        return fail(req, "epoch is required");
    }
    /* Seconds, parsed as 64-bit: time_t is 64-bit here and a 32-bit parse
     * would turn a date past 2038 into one in the past, which is the exact
     * class of mistake the sane window exists to catch. */
    char *end = NULL;
    long long epoch = strtoll(value, &end, 10);
    if (end == value || (end && *end)) {
        return fail(req, "epoch must be a whole number of seconds");
    }
    /* Which refusal it was, because they are different things to tell a
     * person: a time outside the window is theirs to correct, and a clock
     * already set from somewhere better is not a problem at all. The first
     * version of this said both at once and could not say which, which the
     * bench promptly demonstrated. */
    switch (observore_clock_set((time_t)epoch, OBSERVORE_CLOCK_PERSON)) {
    case OBSERVORE_CLOCK_OUT_OF_RANGE:
        return fail(req, "that time is outside the window a running device "
                         "can be in");
    case OBSERVORE_CLOCK_WORSE:
        return fail(req, "the clock is already set from a better source");
    case OBSERVORE_CLOCK_TAKE:
        break;
    }
    return ok(req);
}

static esp_err_t monitors_get_handler(httpd_req_t *req)
{
    char body[768];
    observore_jbuf_t jb;
    observore_jb_init(&jb, body, sizeof(body), 2);
    observore_jb_printf(&jb, "{\"off\":%d,\"monitors\":[",
                        observore_monitors_off_count());
    bool first = true;
    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        if (!observore_monitors_can_toggle((observore_class_t)c)) {
            continue;
        }
        observore_jb_printf(&jb, "%s{\"class\":\"%s\",\"on\":%s,\"protected\":%s}",
            first ? "" : ",", observore_class_name(c),
            observore_monitors_enabled((observore_class_t)c) ? "true" : "false",
            observore_mute_class_needs_address_rule((observore_class_t)c)
                ? "true" : "false");
        first = false;
    }
    observore_jb_close(&jb, "]}");
    return send_json(req, body);
}

/* Switching a monitor off is a deliberate human act and this is the only way
 * to do it. Nothing automatic -- not a baseline, not the census -- has a path
 * to observore_monitors_set(), and that is the rule being kept by keeping it
 * that way. */
static esp_err_t monitors_set_handler(httpd_req_t *req)
{
    char q[96], name[32], onv[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "class", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(q, "on", onv, sizeof(onv)) != ESP_OK) {
        return fail(req, "need class= and on=");
    }
    bool on = (onv[0] == '1' || onv[0] == 't' || onv[0] == 'y');

    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        if (strcmp(name, observore_class_name(c)) != 0) {
            continue;
        }
        if (!observore_monitors_set((observore_class_t)c, on)) {
            return fail(req, "that monitor cannot be switched");
        }
        /* Said in the log as well as answered, because switching a monitor
         * off is the kind of change somebody should be able to find later
         * without having thought to look at the time. */
        ESP_LOGW(TAG, "monitor %s switched %s by the console",
                 observore_class_name(c), on ? "on" : "off");
        return send_json(req, "{\"ok\":true}");
    }
    return fail(req, "no such monitor");
}

static esp_err_t update_handler(httpd_req_t *req)
{
    esp_err_t err = observore_update_install();
    if (err != ESP_OK) {
        /* The reason is the useful part -- "nothing newer", "this build does
         * not name its board" and "already running" are different problems. */
        return fail(req, observore_update_error()[0]
                         ? observore_update_error()
                         : esp_err_to_name(err));
    }
    return ok(req);
}

/* Ask for a version check now. The check runs from the main loop on the
 * uplink, so this only queues it; the page watches `checking` and `checked_s`
 * in the status for the answer. Cheap enough not to rate-limit: one small
 * document over a connection the device is already holding open. */
/* The backlight, for a board with a panel and no touch to reach. Refused
 * rather than ignored where there is no panel, so a console that offers the
 * control is a console whose device has one. */
static esp_err_t bright_handler(httpd_req_t *req)
{
    char value[8];
    if (!query_param(req, "step", value, sizeof(value))) {
        return fail(req, "step is required");
    }
    if (observore_display_brightness() < 0) {
        return fail(req, "this build has no screen");
    }
    int step = atoi(value);
    int top = observore_display_has_light_sensor() ? OBSERVORE_BRIGHT_AUTO
                                                   : OBSERVORE_BRIGHT_STEPS - 1;
    if (step < 0 || step > top) {
        return fail(req, "step is out of range");
    }
    observore_display_set_brightness(step);
    return ok(req);
}

static esp_err_t update_check_handler(httpd_req_t *req)
{
    observore_update_check_now();
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
    /* The scratch is taken by whichever handler needs it -- see
     * scratch_body() -- and not here. It is still taken on a request rather
     * than when the server starts, and released when the window closes: the
     * server is up for every uplink window whether or not anybody is looking,
     * and on a board with no PSRAM its scratch is the largest single block in
     * the heap. Held from the start of the window, it left no contiguous room
     * for the certificate check behind the daily update request: 31 KB free,
     * and a 4,437-byte allocation for an RSA signature failing anyway. A
     * console nobody opens costs nothing, and a console somebody is using is
     * worth more than a version check that can wait for the next window. */
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
        {"/api/history",   HTTP_GET,  history_handler,    false},
        {"/api/history",   HTTP_POST, history_clear_handler, false},
        {"/api/clear",     HTTP_POST, clear_handler,      false},
        {"/api/mutes",     HTTP_GET,  mutes_handler,      false},
        {"/api/mute",      HTTP_POST, mute_handler,       false},
        {"/api/unmute",    HTTP_POST, unmute_handler,     false},
        {"/api/baseline",  HTTP_POST, baseline_handler,   false},
        {"/api/update",    HTTP_POST, update_handler,     false},
        {"/api/update/check", HTTP_POST, update_check_handler, false},
        {"/api/bright",    HTTP_POST, bright_handler,     false},
        {"/api/heap",      HTTP_GET,  heap_handler,       false},
        {"/api/census",    HTTP_GET,  census_handler,     false},
        {"/api/time",      HTTP_POST, time_handler,       false},
        {"/api/monitors",  HTTP_GET,  monitors_get_handler, false},
        {"/api/monitors",  HTTP_POST, monitors_set_handler, false},
        {"/api/netcfg",    HTTP_GET,  netcfg_get_handler, false},
        {"/api/netcfg",    HTTP_POST, netcfg_set_handler, false},
        {"/api/notify",    HTTP_GET,  notify_get_handler, false},
        {"/api/notify",    HTTP_POST, notify_set_handler, false},
    };
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    /* Close with a reset, not a FIN that waits for a reply.
     *
     * Three overnight runs ended with the internal heap at a few hundred bytes
     * and the console unable to start, and the serial log of the third showed
     * why: a browser tab left open on the console reconnects each uplink
     * window, asks for the history, and stops reading. The send fails with
     * EAGAIN, the handler gives up, the server is stopped when the device
     * leaves the uplink -- and lwIP keeps the unsent 9 KB queued behind a FIN
     * it retransmits for minutes to a client that is not there, then does it
     * again next window. Three windows cost 26 KB for good.
     *
     * A zero linger turns close into a reset: the connection and everything
     * queued on it are freed at once. Nothing is lost that was going to arrive
     * anyway, since the client had stopped reading. */
    cfg.enable_so_linger = true;
    cfg.linger_timeout = 0;
    /* And fewer of them, purged sooner. A browser opens about six connections
     * and a backgrounded tab stops reading on all of them; with the default
     * seven allowed and five seconds before a blocked send gives up, six
     * stalled responses sit in lwIP's send buffers at once and the heap is
     * gone before the reset above ever gets its chance. Three sockets is
     * plenty for one person reading a page, LRU purge evicts the oldest the
     * moment a fourth arrives, and a send that cannot make progress in two
     * seconds is to a client that has stopped listening. */
    cfg.max_open_sockets = 3;
    cfg.send_wait_timeout = 2;
    cfg.recv_wait_timeout = 2;
    cfg.stack_size = 8192;   /* the JSON handlers are not frugal */
    /* Sized from the table rather than left at the default of 8.  Overflowing
     * it makes httpd_register_uri_handler fail and the route simply not exist,
     * which surfaces as a 405 on a route that is plainly in the source. */
    cfg.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
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
