#include <string.h>

#include "observore_detect.h"
#include "observore_mute.h"

#ifdef OBSERVORE_HOST_TEST
/* The host build exercises the matching and list logic without NVS. */
#define MUTE_LOCK()   do {} while (0)
#define MUTE_UNLOCK() do {} while (0)
static void mute_load(void) {}
static void mute_save(void) {}
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "observore.mute";
static SemaphoreHandle_t s_lock;
#define MUTE_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define MUTE_UNLOCK() xSemaphoreGiveRecursive(s_lock)

#include "observore_nvs.h"
#define NVS_NAMESPACE OBSERVORE_NVS_NAMESPACE
#define NVS_KEY       "mutes"
#endif

static observore_mute_rule_t s_rules[OBSERVORE_MUTE_MAX];
static size_t            s_count;
static uint32_t          s_suppressed;

/* ------------------------------------------------------------------ */
/* Persistence                                                        */
/* ------------------------------------------------------------------ */

#ifndef OBSERVORE_HOST_TEST
static void mute_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;  /* nothing stored yet -- not an error */
    }
    size_t len = sizeof(s_rules);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, s_rules, &len);
    nvs_close(h);

    if (err == ESP_OK && len % sizeof(observore_mute_rule_t) == 0) {
        s_count = len / sizeof(observore_mute_rule_t);
        if (s_count > OBSERVORE_MUTE_MAX) {
            s_count = OBSERVORE_MUTE_MAX;
        }
        ESP_LOGI(TAG, "loaded %zu mute rules", s_count);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* A short or corrupt blob would otherwise be read as garbage rules
         * that silently suppress real detections. */
        ESP_LOGW(TAG, "discarding stored mutes: %s", esp_err_to_name(err));
        memset(s_rules, 0, sizeof(s_rules));
        s_count = 0;
    }
}

static void mute_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, NVS_KEY, s_rules, s_count * sizeof(observore_mute_rule_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist mutes: %s", esp_err_to_name(err));
    }
}
#endif

void observore_mute_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    MUTE_LOCK();
    s_count = 0;
    s_suppressed = 0;
    memset(s_rules, 0, sizeof(s_rules));
    mute_load();
    MUTE_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Matching                                                           */
/* ------------------------------------------------------------------ */

bool observore_mute_class_is_protected(observore_class_t cls)
{
    /* Cameras and fleet telematics are deliberately unprotected: those are
     * street furniture, and muting a whole brand of them is the point. */
    return observore_class_desc(cls)->protected_cls;
}

bool observore_mute_matches(const uint8_t mac[OBSERVORE_MAC_LEN], observore_class_t cls,
                        const char *name, uint32_t fingerprint)
{
    if (!mac) {
        return false;
    }
    bool hit = false;

    MUTE_LOCK();
    for (size_t i = 0; i < s_count && !hit; i++) {
        const observore_mute_rule_t *r = &s_rules[i];
        switch (r->kind) {
            case OBSERVORE_MUTE_MAC:
                hit = memcmp(r->mac, mac, OBSERVORE_MAC_LEN) == 0;
                break;
            case OBSERVORE_MUTE_OUI:
                hit = memcmp(r->mac, mac, 3) == 0;
                break;
            case OBSERVORE_MUTE_CLASS:
                /* Never let a class rule swallow unclassified traffic, or
                 * muting "camera" would also disable the follower heuristic. */
                hit = cls != OBSERVORE_CLASS_UNKNOWN && r->cls == (uint8_t)cls;
                break;
            case OBSERVORE_MUTE_NAME:
                hit = name && observore_contains_ci(name, r->ssid);
                break;
            case OBSERVORE_MUTE_FINGERPRINT:
                /* The safety rule, enforced here rather than left to the
                 * caller: a fingerprint identifies a kind of device, so it
                 * must never be able to silence a threat. */
                hit = fingerprint != 0 && r->fingerprint == fingerprint &&
                      !observore_mute_class_is_protected(cls);
                break;
            default:
                break;
        }
    }
    if (hit) {
        s_suppressed++;
    }
    MUTE_UNLOCK();
    return hit;
}

/* ------------------------------------------------------------------ */
/* List management                                                    */
/* ------------------------------------------------------------------ */

static bool same_rule(const observore_mute_rule_t *a, const observore_mute_rule_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case OBSERVORE_MUTE_MAC:   return memcmp(a->mac, b->mac, OBSERVORE_MAC_LEN) == 0;
        case OBSERVORE_MUTE_OUI:   return memcmp(a->mac, b->mac, 3) == 0;
        case OBSERVORE_MUTE_CLASS: return a->cls == b->cls;
        case OBSERVORE_MUTE_NAME:  return strcmp(a->ssid, b->ssid) == 0;
        case OBSERVORE_MUTE_FINGERPRINT: return a->fingerprint == b->fingerprint;
        default:               return false;
    }
}

static esp_err_t mute_add(const observore_mute_rule_t *rule, bool *added,
                         bool persist)
{
    if (!rule || rule->kind >= OBSERVORE_MUTE_KIND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_NAME && rule->ssid[0] == '\0') {
        /* An empty substring matches everything. */
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_FINGERPRINT && rule->fingerprint == 0) {
        /* 0 means "no fingerprint", so such a rule would be meaningless. */
        return ESP_ERR_INVALID_ARG;
    }
    if (rule->kind == OBSERVORE_MUTE_CLASS &&
        (rule->cls == OBSERVORE_CLASS_UNKNOWN || rule->cls >= OBSERVORE_CLASS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (added) {
        *added = false;
    }

    esp_err_t err = ESP_OK;
    MUTE_LOCK();
    for (size_t i = 0; i < s_count; i++) {
        if (same_rule(&s_rules[i], rule)) {
            MUTE_UNLOCK();
            return ESP_OK;  /* idempotent */
        }
    }
    if (s_count >= OBSERVORE_MUTE_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_rules[s_count++] = *rule;
        if (added) {
            *added = true;
        }
        if (persist) {
            mute_save();
        }
    }
    MUTE_UNLOCK();
    return err;
}

esp_err_t observore_mute_add(const observore_mute_rule_t *rule, bool *added)
{
    return mute_add(rule, added, true);
}

esp_err_t observore_mute_add_deferred(const observore_mute_rule_t *rule,
                                      bool *added)
{
    return mute_add(rule, added, false);
}

void observore_mute_save(void)
{
    MUTE_LOCK();
    mute_save();
    MUTE_UNLOCK();
}

esp_err_t observore_mute_remove(size_t index)
{
    esp_err_t err = ESP_OK;
    MUTE_LOCK();
    if (index >= s_count) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        memmove(&s_rules[index], &s_rules[index + 1],
                (s_count - index - 1) * sizeof(observore_mute_rule_t));
        s_count--;
        memset(&s_rules[s_count], 0, sizeof(observore_mute_rule_t));
        mute_save();
    }
    MUTE_UNLOCK();
    return err;
}

esp_err_t observore_mute_clear(void)
{
    MUTE_LOCK();
    s_count = 0;
    memset(s_rules, 0, sizeof(s_rules));
    mute_save();
    MUTE_UNLOCK();
    return ESP_OK;
}

size_t observore_mute_count(void)
{
    MUTE_LOCK();
    size_t n = s_count;
    MUTE_UNLOCK();
    return n;
}

bool observore_mute_get(size_t index, observore_mute_rule_t *out)
{
    if (!out) {
        return false;
    }
    MUTE_LOCK();
    bool ok = index < s_count;
    if (ok) {
        *out = s_rules[index];
    }
    MUTE_UNLOCK();
    return ok;
}

size_t observore_mute_list(observore_mute_rule_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    MUTE_LOCK();
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_rules, n * sizeof(observore_mute_rule_t));
    MUTE_UNLOCK();
    return n;
}

uint32_t observore_mute_suppressed(void)
{
    return s_suppressed;
}

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                    */
/* ------------------------------------------------------------------ */

const char *observore_mute_kind_name(observore_mute_kind_t kind)
{
    switch (kind) {
        case OBSERVORE_MUTE_MAC:   return "mac";
        case OBSERVORE_MUTE_OUI:   return "oui";
        case OBSERVORE_MUTE_CLASS: return "class";
        case OBSERVORE_MUTE_NAME:  return "name";
        case OBSERVORE_MUTE_FINGERPRINT: return "fingerprint";
        default:               return "?";
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool observore_mute_parse_mac(const char *text, uint8_t *out, size_t want)
{
    if (!text || !out || want == 0 || want > OBSERVORE_MAC_LEN) {
        return false;
    }
    size_t got = 0;
    const char *p = text;

    while (got < want) {
        while (*p == ':' || *p == '-' || *p == '.') {
            p++;
        }
        int hi = hex_nibble(*p);
        if (hi < 0) {
            return false;
        }
        int lo = hex_nibble(*(p + 1));
        if (lo < 0) {
            return false;
        }
        out[got++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }

    /* Reject trailing rubbish so a typo is an error rather than a rule that
     * silently matches the wrong device. */
    while (*p == ':' || *p == '-' || *p == '.') {
        p++;
    }
    return *p == '\0';
}

bool observore_mute_parse_class(const char *name, observore_class_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
        if (strcmp(name, observore_class_name(c)) == 0) {
            *out = (observore_class_t)c;
            return true;
        }
    }
    return false;
}
