#include <stdio.h>
#include <string.h>

#include "argus_netcfg.h"

#ifdef ARGUS_HOST_TEST
#define NETCFG_LOCK()   do {} while (0)
#define NETCFG_UNLOCK() do {} while (0)
#define ARGUS_CFG_SSID     ""
#define ARGUS_CFG_PASSWORD ""
static void netcfg_load(void) {}
static void netcfg_save(void) {}
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "argus.netcfg";
static SemaphoreHandle_t s_lock;
#define NETCFG_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define NETCFG_UNLOCK() xSemaphoreGiveRecursive(s_lock)

#define NVS_NAMESPACE "argus"
#define NVS_KEY_SSID  "sta_ssid"
#define NVS_KEY_PASS  "sta_pass"

#define ARGUS_CFG_SSID     CONFIG_ARGUS_WIFI_SSID
#define ARGUS_CFG_PASSWORD CONFIG_ARGUS_WIFI_PASSWORD
#endif

static argus_netcfg_t s_cfg;

/* ------------------------------------------------------------------ */

#ifndef ARGUS_HOST_TEST
static void netcfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_cfg.ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_cfg.ssid, &len) != ESP_OK) {
        s_cfg.ssid[0] = '\0';
    }
    len = sizeof(s_cfg.password);
    if (nvs_get_str(h, NVS_KEY_PASS, s_cfg.password, &len) != ESP_OK) {
        s_cfg.password[0] = '\0';
    }
    nvs_close(h);

    if (s_cfg.ssid[0]) {
        ESP_LOGI(TAG, "using network \"%s\" from NVS", s_cfg.ssid);
    }
}

static void netcfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, NVS_KEY_SSID, s_cfg.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, s_cfg.password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist credentials: %s", esp_err_to_name(err));
    }
}
#endif

void argus_netcfg_init(void)
{
#ifndef ARGUS_HOST_TEST
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
#endif
    NETCFG_LOCK();
    memset(&s_cfg, 0, sizeof(s_cfg));
    netcfg_load();

    /* Seed from the build-time defaults only when NVS has nothing, so a
     * credential set through the console is never silently reverted by a
     * reflash of the same firmware. */
    if (s_cfg.ssid[0] == '\0' && ARGUS_CFG_SSID[0] != '\0') {
        snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", ARGUS_CFG_SSID);
        snprintf(s_cfg.password, sizeof(s_cfg.password), "%s", ARGUS_CFG_PASSWORD);
#ifndef ARGUS_HOST_TEST
        ESP_LOGI(TAG, "using network \"%s\" from build configuration", s_cfg.ssid);
#endif
    }
    NETCFG_UNLOCK();
}

bool argus_netcfg_get(argus_netcfg_t *out)
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

bool argus_netcfg_is_set(void)
{
    NETCFG_LOCK();
    bool set = s_cfg.ssid[0] != '\0';
    NETCFG_UNLOCK();
    return set;
}

bool argus_netcfg_ssid(char *out, size_t len)
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

bool argus_netcfg_valid(const char *ssid, const char *password, const char **why)
{
    const char *ignored = NULL;
    if (!why) {
        why = &ignored;
    }
    if (!ssid || ssid[0] == '\0') {
        *why = "ssid is required";
        return false;
    }
    if (strlen(ssid) >= ARGUS_SSID_LEN) {
        *why = "ssid is longer than 32 characters";
        return false;
    }
    if (password && strlen(password) >= ARGUS_PASSWORD_LEN) {
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

esp_err_t argus_netcfg_set(const char *ssid, const char *password)
{
    if (!argus_netcfg_valid(ssid, password, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    NETCFG_LOCK();
    snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", ssid);
    snprintf(s_cfg.password, sizeof(s_cfg.password), "%s",
             password ? password : "");
    netcfg_save();
    NETCFG_UNLOCK();
    return ESP_OK;
}

esp_err_t argus_netcfg_clear(void)
{
    NETCFG_LOCK();
    memset(&s_cfg, 0, sizeof(s_cfg));
    netcfg_save();
    NETCFG_UNLOCK();
    return ESP_OK;
}
