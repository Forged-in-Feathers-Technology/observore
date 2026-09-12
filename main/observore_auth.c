#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "observore_auth.h"
#include "observore_netcfg.h"
#include "observore_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "observore.auth";

#define TOKEN_CHARS   32                    /* 128 bits of hex */
#define MAX_SESSIONS  4
#define SESSION_TTL_US (2 * 60 * 60 * 1000000LL)   /* idle timeout */
#define COOKIE_NAME   "observore_session"

typedef struct {
    char    token[TOKEN_CHARS + 1];
    int64_t expires_us;
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static uint32_t  s_failures;

void observore_auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_failures = 0;
}

/* Compare without an early exit, so the time taken does not describe how much
 * of the secret was right. */
static bool equal_ct(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    }
    return diff == 0;
}

static void new_token(char *out)
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < TOKEN_CHARS; i++) {
        out[i] = HEX[esp_random() & 0x0F];
    }
    out[TOKEN_CHARS] = '\0';
}

/* Read our cookie out of the request, if present. */
static bool cookie_token(httpd_req_t *req, char *out, size_t len)
{
    return httpd_req_get_cookie_val(req, COOKIE_NAME, out, &len) == ESP_OK;
}

static session_t *find_session(const char *token, int64_t now)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0') {
            continue;
        }
        if (s_sessions[i].expires_us <= now) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            continue;
        }
        if (equal_ct(s_sessions[i].token, token)) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

bool observore_auth_enforced(void)
{
#if !CONFIG_OBSERVORE_CONSOLE_AUTH
    return false;
#else
    /* The SoftAP link is already authenticated by WPA2 with the same password,
     * so challenging again there only gets in the way of first-time setup. */
    return observore_wifi_mode() != OBSERVORE_MODE_CONSOLE;
#endif
}

bool observore_auth_ok(httpd_req_t *req)
{
    if (!observore_auth_enforced()) {
        return true;
    }
    char token[TOKEN_CHARS + 1] = {0};
    if (!cookie_token(req, token, sizeof(token))) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    session_t *s = find_session(token, now);
    if (!s) {
        return false;
    }
    s->expires_us = now + SESSION_TTL_US;   /* idle timeout, not absolute */
    return true;
}

esp_err_t observore_auth_login(httpd_req_t *req, const char *password)
{
    const char *expected = observore_netcfg_ap_password();
    if (!password || !expected || !*expected) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!equal_ct(password, expected)) {
        /* Slow down guessing. An ESP32 cannot afford a real key-derivation
         * function, so the cost has to come from somewhere; a delay that grows
         * with consecutive failures is cheap and enough against a script. */
        s_failures++;
        uint32_t delay_ms = s_failures * 250;
        if (delay_ms > 4000) {
            delay_ms = 4000;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        ESP_LOGW(TAG, "console login failed (%" PRIu32 " in a row)", s_failures);
        return ESP_ERR_INVALID_ARG;
    }
    s_failures = 0;

    int64_t now = esp_timer_get_time();
    session_t *slot = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0' || s_sessions[i].expires_us <= now) {
            slot = &s_sessions[i];
            break;
        }
    }
    if (!slot) {
        /* All in use: evict the one closest to expiring rather than refusing
         * to let somebody in. */
        slot = &s_sessions[0];
        for (int i = 1; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].expires_us < slot->expires_us) {
                slot = &s_sessions[i];
            }
        }
    }
    new_token(slot->token);
    slot->expires_us = now + SESSION_TTL_US;

    /* Static, not a local: httpd_resp_set_hdr() stores the POINTER and the
     * value must still be valid when the response is sent, which is after this
     * function returns.  A stack buffer here produced a login that reported
     * success and set no cookie at all.  esp_http_server dispatches from a
     * single task, so one buffer is enough.
     *
     * HttpOnly keeps it away from scripts; SameSite=Strict stops another site
     * driving the console through the viewer's browser.  No Secure flag: the
     * console is plain HTTP today, and setting it would stop the cookie
     * working at all. */
    static char cookie[128];
    snprintf(cookie, sizeof(cookie),
             COOKIE_NAME "=%s; Path=/; Max-Age=7200; HttpOnly; SameSite=Strict",
             slot->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "console login");
    return ESP_OK;
}

void observore_auth_logout(httpd_req_t *req)
{
    char token[TOKEN_CHARS + 1] = {0};
    if (cookie_token(req, token, sizeof(token))) {
        session_t *s = find_session(token, esp_timer_get_time());
        if (s) {
            memset(s, 0, sizeof(*s));
        }
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}
