#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "argus_types.h"

#ifndef ARGUS_HOST_TEST
#include "esp_err.h"
#endif

/* Wi-Fi station credentials for the uplink mode.
 *
 * Credentials never live in a tracked file.  They come from one of two places,
 * runtime first:
 *
 *   1. NVS, set through the console.  Nothing touches the repo at all.
 *   2. Kconfig defaults, which land in sdkconfig -- which is gitignored.
 *      tools/check_no_secrets.sh enforces that they stay out of the tracked
 *      sdkconfig.defaults.
 *
 * NVS wins when both are present, so a device can be re-pointed at a different
 * network without a rebuild. */

#define ARGUS_SSID_LEN     33   /* 32 + NUL */
#define ARGUS_PASSWORD_LEN 65   /* 64 + NUL */

typedef struct {
    char ssid[ARGUS_SSID_LEN];
    char password[ARGUS_PASSWORD_LEN];
} argus_netcfg_t;

void argus_netcfg_init(void);

/* Returns false when no network has been configured, by either route. */
bool argus_netcfg_get(argus_netcfg_t *out);

/* True when an SSID is configured.  Cheaper than get() and, unlike get(), it
 * never puts a password on the caller's stack. */
bool argus_netcfg_is_set(void);

/* Copies only the SSID.  This is what the API is allowed to report -- the
 * password is write-only from outside the device. */
bool argus_netcfg_ssid(char *out, size_t len);

esp_err_t argus_netcfg_set(const char *ssid, const char *password);
esp_err_t argus_netcfg_clear(void);

/* Validation, exposed so the API can reject input with a useful message
 * rather than storing something that can never associate. */
bool argus_netcfg_valid(const char *ssid, const char *password,
                        const char **why);
