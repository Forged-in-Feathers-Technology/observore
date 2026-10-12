#include "observore_tz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool observore_tz_valid(const char *tz)
{
    if (!tz) {
        return false;
    }
    size_t n = strlen(tz);
    /* "UTC0" is the shortest thing worth accepting; POSIX wants at least a
     * three-character abbreviation. */
    if (n < 3 || n >= OBSERVORE_TZ_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = tz[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '+' || c == '-' || c == ':' || c == ',' ||
                  c == '.' || c == '/' || c == '<' || c == '>';
        if (!ok) {
            return false;
        }
    }
    /* It has to start with a zone abbreviation, not an offset: "5EST" is not a
     * TZ string, and newlib's reading of one is not worth discovering in the
     * field. The angle-bracket form "<+05>-5" is the one exception POSIX
     * allows. */
    if (tz[0] != '<' && !((tz[0] >= 'A' && tz[0] <= 'Z') ||
                          (tz[0] >= 'a' && tz[0] <= 'z'))) {
        return false;
    }
    return true;
}

bool observore_tz_from_offset(int east_minutes, char *out, size_t out_len)
{
    if (!out || out_len < 12) {
        return false;
    }
    /* The range real zones occupy. Beyond it the string would be accepted by
     * tzset and mean something nobody intended. */
    if (east_minutes < -12 * 60 || east_minutes > 14 * 60) {
        return false;
    }

    /* POSIX writes how far the zone is BEHIND UTC, so the sign flips. This is
     * the line the host tests exist for. */
    int posix = -east_minutes;
    const char *sign = posix < 0 ? "-" : "+";
    int mag = posix < 0 ? -posix : posix;
    int h = mag / 60, m = mag % 60;

    int wrote;
    if (posix == 0) {
        wrote = snprintf(out, out_len, "UTC0");
    } else if (m == 0) {
        wrote = snprintf(out, out_len, "UTC%s%d", sign, h);
    } else {
        wrote = snprintf(out, out_len, "UTC%s%d:%02d", sign, h, m);
    }
    return wrote > 0 && (size_t)wrote < out_len;
}

int observore_tz_offset_of(const char *tz)
{
    if (!observore_tz_valid(tz)) {
        return OBSERVORE_TZ_NOT_FIXED;
    }
    /* Only the form this module writes is read back. Anything else -- a zone
     * with daylight-saving rules, or somebody's hand-written string -- has no
     * single offset to report, and guessing one would put a number on the
     * screen that the device does not actually keep time by. */
    if (strncmp(tz, "UTC", 3) != 0) {
        return OBSERVORE_TZ_NOT_FIXED;
    }
    const char *p = tz + 3;
    if (strcmp(p, "0") == 0) {
        return 0;
    }
    int sign;
    if (*p == '-') {
        sign = -1;
    } else if (*p == '+') {
        sign = 1;
    } else {
        return OBSERVORE_TZ_NOT_FIXED;
    }
    p++;
    if (*p < '0' || *p > '9') {
        return OBSERVORE_TZ_NOT_FIXED;
    }
    int h = 0;
    while (*p >= '0' && *p <= '9') {
        h = h * 10 + (*p++ - '0');
    }
    int m = 0;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') {
            return OBSERVORE_TZ_NOT_FIXED;
        }
        while (*p >= '0' && *p <= '9') {
            m = m * 10 + (*p++ - '0');
        }
    }
    if (*p != '\0' || h > 14 || m > 59) {
        return OBSERVORE_TZ_NOT_FIXED;
    }
    /* Back the other way: the string counts west, the answer counts east. */
    return -sign * (h * 60 + m);
}

#ifndef OBSERVORE_HOST_TEST

#include "esp_log.h"
#include "sdkconfig.h"

#include "observore_nvs.h"

static const char *TAG = "observore.tz";

static char s_tz[OBSERVORE_TZ_MAX];

static void apply(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
}

const char *observore_tz_get(void)
{
    return s_tz[0] ? s_tz : CONFIG_OBSERVORE_TZ;
}

bool observore_tz_set(const char *tz)
{
    if (!observore_tz_valid(tz)) {
        return false;
    }
    /* Copied, applied, then stored. Applying before the write means the device
     * is already keeping time the way the owner asked even if the flash write
     * fails, and the warning says what will not survive. */
    snprintf(s_tz, sizeof(s_tz), "%s", tz);
    apply();

    const observore_nvs_item_t item = {
        .key = "tz", .type = OBSERVORE_NVS_STR,
        .buf = s_tz, .len = strlen(s_tz) + 1,
    };
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the timezone; it will go back to %s at "
                      "the next boot", CONFIG_OBSERVORE_TZ);
        return false;
    }
    /* Said plainly, because it changes where the census draws a day boundary
     * and the days already counted keep the boundary they were counted with. */
    ESP_LOGI(TAG, "timezone is now %s; days already counted keep the "
                  "boundary they were counted with", s_tz);
    return true;
}

void observore_tz_init(void)
{
    char stored[OBSERVORE_TZ_MAX] = {0};
    observore_nvs_item_t item = {
        .key = "tz", .type = OBSERVORE_NVS_STR,
        .buf = stored, .len = sizeof(stored),
    };
    observore_nvs_read(&item, 1);

    if (item.found && observore_tz_valid(stored)) {
        snprintf(s_tz, sizeof(s_tz), "%s", stored);
    } else {
        if (item.found && stored[0]) {
            ESP_LOGW(TAG, "the stored timezone is not one this build will "
                          "accept; using %s", CONFIG_OBSERVORE_TZ);
        }
        snprintf(s_tz, sizeof(s_tz), "%s", CONFIG_OBSERVORE_TZ);
    }
    apply();
    ESP_LOGI(TAG, "timezone %s", s_tz);
}

#endif /* OBSERVORE_HOST_TEST */
