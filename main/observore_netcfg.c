#include <stdio.h>
#include <string.h>

#include "observore_netcfg.h"

#ifndef OBSERVORE_HOST_TEST
#include "esp_random.h"
#endif

#ifdef OBSERVORE_HOST_TEST
#define NETCFG_LOCK()   do {} while (0)
#define NETCFG_UNLOCK() do {} while (0)
#define OBSERVORE_CFG_SSID     ""
#define OBSERVORE_CFG_PASSWORD ""
static void netcfg_load(void) {}
static void netcfg_save(void) {}
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "observore_nvs.h"
#include "sdkconfig.h"

static const char *TAG = "observore.netcfg";
static SemaphoreHandle_t s_lock;
#define NETCFG_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define NETCFG_UNLOCK() xSemaphoreGiveRecursive(s_lock)

#include "observore_nvs.h"

#define OBSERVORE_CFG_SSID     CONFIG_OBSERVORE_WIFI_SSID
#define OBSERVORE_CFG_PASSWORD CONFIG_OBSERVORE_WIFI_PASSWORD
#endif

static observore_netcfg_t s_cfg;

/* 12 characters from a 31-symbol alphabet is about 59 bits -- far beyond what
 * a WPA2 handshake capture can be brute-forced through, and short enough to
 * read off a serial log and type on a phone.  Ambiguous glyphs are left out
 * for the same reason. */
#define AP_PASSWORD_LEN 12
static const char AP_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
/* Sized for any WPA2 passphrase, since a build-time override may be far
 * longer than the generated one. */
static char s_ap_password[OBSERVORE_PASSWORD_LEN];

#ifndef OBSERVORE_HOST_TEST
static void ap_password_init(void);
#endif

/* ------------------------------------------------------------------ */

#ifndef OBSERVORE_HOST_TEST
static void netcfg_load(void)
{
    observore_nvs_item_t items[] = {
        {.key = "sta_ssid", .type = OBSERVORE_NVS_STR,
         .buf = s_cfg.ssid,     .len = sizeof(s_cfg.ssid)},
        {.key = "sta_pass", .type = OBSERVORE_NVS_STR,
         .buf = s_cfg.password, .len = sizeof(s_cfg.password)},
    };
    observore_nvs_read(items, OBSERVORE_ARRLEN(items));
    if (!items[0].found) {
        s_cfg.ssid[0] = '\0';
    }
    if (!items[1].found) {
        s_cfg.password[0] = '\0';
    }
    if (s_cfg.ssid[0]) {
        ESP_LOGI(TAG, "using network \"%s\" from NVS", s_cfg.ssid);
    }
}

static void netcfg_save(void)
{
    /* One batch: an SSID and its password must land together, or a power cut
     * between two commits leaves a network configured with the wrong
     * credential. */
    const observore_nvs_item_t items[] = {
        {.key = "sta_ssid", .type = OBSERVORE_NVS_STR, .buf = s_cfg.ssid},
        {.key = "sta_pass", .type = OBSERVORE_NVS_STR, .buf = s_cfg.password},
    };
    observore_nvs_write(items, OBSERVORE_ARRLEN(items));
}
#endif

void observore_netcfg_init(void)
{
#ifndef OBSERVORE_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    NETCFG_LOCK();
    memset(&s_cfg, 0, sizeof(s_cfg));
    netcfg_load();
#ifndef OBSERVORE_HOST_TEST
    ap_password_init();
#endif

    /* Seed from the build-time defaults only when NVS has nothing, so a
     * credential set through the console is never silently reverted by a
     * reflash of the same firmware. */
    if (s_cfg.ssid[0] == '\0' && OBSERVORE_CFG_SSID[0] != '\0') {
        snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", OBSERVORE_CFG_SSID);
        snprintf(s_cfg.password, sizeof(s_cfg.password), "%s", OBSERVORE_CFG_PASSWORD);
#ifndef OBSERVORE_HOST_TEST
        ESP_LOGI(TAG, "using network \"%s\" from build configuration", s_cfg.ssid);
#endif
    }
    NETCFG_UNLOCK();
}

bool observore_netcfg_get(observore_netcfg_t *out)
{
    if (!out) {
        return false;
    }
    NETCFG_LOCK();
    *out = s_cfg;
    bool set = s_cfg.ssid[0] != '\0';
    NETCFG_UNLOCK();
    return set;
}

bool observore_netcfg_is_set(void)
{
    NETCFG_LOCK();
    bool set = s_cfg.ssid[0] != '\0';
    NETCFG_UNLOCK();
    return set;
}

#ifndef OBSERVORE_HOST_TEST
static void ap_password_init(void)
{
    /* An explicit build-time password wins, and is the caller's problem. */
    if (CONFIG_OBSERVORE_AP_PASSWORD[0] != '\0') {
        snprintf(s_ap_password, sizeof(s_ap_password), "%s",
                 CONFIG_OBSERVORE_AP_PASSWORD);
        ESP_LOGW(TAG, "console password comes from the build configuration, so "
                      "every device built from this firmware shares it");
        return;
    }

    observore_nvs_item_t stored = {.key = "ap_pass", .type = OBSERVORE_NVS_STR,
                                   .buf = s_ap_password,
                                   .len = sizeof(s_ap_password)};
    observore_nvs_read(&stored, 1);
    if (stored.found && s_ap_password[0] != '\0') {
        return;
    }
    s_ap_password[0] = '\0';

    /* esp_random() is a true hardware RNG once Wi-Fi or Bluetooth is running,
     * which it is by the time this is called. */
    for (size_t i = 0; i < AP_PASSWORD_LEN; i++) {
        s_ap_password[i] = AP_ALPHABET[esp_random() % (sizeof(AP_ALPHABET) - 1)];
    }
    s_ap_password[AP_PASSWORD_LEN] = '\0';

    const observore_nvs_item_t item = {.key = "ap_pass",
                                       .type = OBSERVORE_NVS_STR,
                                       .buf = s_ap_password};
    observore_nvs_write(&item, 1);
    ESP_LOGW(TAG, "generated a console password for this device");
}
#endif

const char *observore_netcfg_ap_password(void)
{
    return s_ap_password;
}

bool observore_netcfg_has_password(void)
{
    NETCFG_LOCK();
    bool set = s_cfg.password[0] != '\0';
    NETCFG_UNLOCK();
    return set;
}

bool observore_netcfg_ssid(char *out, size_t len)
{
    if (!out || len == 0) {
        return false;
    }
    NETCFG_LOCK();
    snprintf(out, len, "%s", s_cfg.ssid);
    bool set = s_cfg.ssid[0] != '\0';
    NETCFG_UNLOCK();
    return set;
}

bool observore_netcfg_valid(const char *ssid, const char *password, const char **why)
{
    const char *ignored = NULL;
    if (!why) {
        why = &ignored;
    }
    if (!ssid || ssid[0] == '\0') {
        *why = "ssid is required";
        return false;
    }
    if (strlen(ssid) >= OBSERVORE_SSID_LEN) {
        *why = "ssid is longer than 32 characters";
        return false;
    }
    if (password && strlen(password) >= OBSERVORE_PASSWORD_LEN) {
        *why = "password is longer than 64 characters";
        return false;
    }
    /* WPA2 requires 8 characters.  An empty password means an open network,
     * which is legitimate; 1-7 characters can never associate, so it is worth
     * rejecting now rather than at connect time. */
    if (password && password[0] != '\0' && strlen(password) < 8) {
        *why = "password must be empty (open network) or at least 8 characters";
        return false;
    }
    return true;
}

esp_err_t observore_netcfg_set(const char *ssid, const char *password)
{
    /* A NULL password means "keep whatever is stored".
     *
     * This is not a convenience.  The console clears its password field after
     * a save, so saving twice used to overwrite a good password with an empty
     * one -- silently reconfiguring a WPA2 network as open.  The driver then
     * refused it with reason 210, "no AP found with compatible security",
     * which reads as though the network were at fault.  Clearing a password
     * now has to be asked for explicitly, by passing an empty string. */
    NETCFG_LOCK();
    const char *effective = password ? password : s_cfg.password;
    if (!observore_netcfg_valid(ssid, effective, NULL)) {
        NETCFG_UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }
    char kept[OBSERVORE_PASSWORD_LEN];
    snprintf(kept, sizeof(kept), "%s", effective);
    snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", ssid);
    snprintf(s_cfg.password, sizeof(s_cfg.password), "%s", kept);
    memset(kept, 0, sizeof(kept));
    netcfg_save();
    NETCFG_UNLOCK();
    return ESP_OK;
}

esp_err_t observore_netcfg_clear(void)
{
    NETCFG_LOCK();
    memset(&s_cfg, 0, sizeof(s_cfg));
    netcfg_save();
    NETCFG_UNLOCK();
    return ESP_OK;
}
