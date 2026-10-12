#include "observore_watchcfg.h"

#include <string.h>

observore_watchcfg_t observore_watchcfg_default(void)
{
    /* The dial as it shipped: marks, no numerals, no digital line. */
    return (observore_watchcfg_t){
        .version  = OBSERVORE_WATCHCFG_VERSION,
        .digital  = 0,
        .numerals = OBSERVORE_NUMERALS_NONE,
        .hour12   = 0,      /* 24-hour, as the face already did */
    };
}

bool observore_watchcfg_valid(const observore_watchcfg_t *cfg)
{
    if (!cfg || cfg->version != OBSERVORE_WATCHCFG_VERSION) {
        return false;
    }
    if (cfg->digital > 1) {
        return false;
    }
    switch (cfg->numerals) {
        case OBSERVORE_NUMERALS_NONE:
        case OBSERVORE_NUMERALS_QUARTERS:
        case OBSERVORE_NUMERALS_ALL:
            break;
        default:
            return false;
    }
    return cfg->hour12 <= 1;
}

uint8_t observore_watchcfg_next_numerals(uint8_t current)
{
    switch (current) {
        case OBSERVORE_NUMERALS_NONE:     return OBSERVORE_NUMERALS_QUARTERS;
        case OBSERVORE_NUMERALS_QUARTERS: return OBSERVORE_NUMERALS_ALL;
        case OBSERVORE_NUMERALS_ALL:      return OBSERVORE_NUMERALS_NONE;
        /* Anything else came from a blob this build does not understand;
         * stepping from it lands somewhere defined rather than compounding
         * the confusion. */
        default:                          return OBSERVORE_NUMERALS_NONE;
    }
}

#ifndef OBSERVORE_HOST_TEST

#include "esp_log.h"

#include "observore_nvs.h"

static const char *TAG = "observore.watchcfg";

static observore_watchcfg_t s_cfg;
static bool                 s_loaded;

observore_watchcfg_t observore_watchcfg_get(void)
{
    if (s_loaded) {
        return s_cfg;
    }

    observore_watchcfg_t stored = {0};
    observore_nvs_item_t item = {
        .key = "watchcfg", .type = OBSERVORE_NVS_BLOB,
        .buf = &stored, .len = sizeof(stored),
    };
    observore_nvs_read(&item, 1);

    /* Three ways to end up with the defaults, and only one of them is worth a
     * line in the log. Nothing stored is the normal first boot; a blob of the
     * wrong length or a version this build does not know is a real
     * disagreement and the owner's settings have just been dropped. */
    if (!item.found) {
        s_cfg = observore_watchcfg_default();
    } else if (item.len != sizeof(stored) ||
               !observore_watchcfg_valid(&stored)) {
        ESP_LOGW(TAG, "stored watch settings are %u bytes version %u, which "
                 "this build cannot draw from -- using the defaults",
                 (unsigned)item.len, (unsigned)stored.version);
        s_cfg = observore_watchcfg_default();
    } else {
        s_cfg = stored;
    }
    s_loaded = true;
    return s_cfg;
}

bool observore_watchcfg_set(const observore_watchcfg_t *cfg)
{
    if (!observore_watchcfg_valid(cfg)) {
        return false;
    }
    /* Kept in memory even if the write fails, so the face the owner just chose
     * is the face they get for this run. The warning says it will not survive,
     * which is the part they cannot see. */
    s_cfg    = *cfg;
    s_loaded = true;

    const observore_nvs_item_t item = {
        .key = "watchcfg", .type = OBSERVORE_NVS_BLOB,
        .buf = (void *)&s_cfg, .len = sizeof(s_cfg),
    };
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the watch settings; they will go back "
                      "to the defaults at the next boot");
        return false;
    }
    return true;
}

#else  /* OBSERVORE_HOST_TEST */

/* No NVS on the host. The tests exercise the validation and the cycle, which
 * is the part with decisions in it; the store is two calls to a helper that
 * has its own tests. */
observore_watchcfg_t observore_watchcfg_get(void)
{
    return observore_watchcfg_default();
}

bool observore_watchcfg_set(const observore_watchcfg_t *cfg)
{
    return observore_watchcfg_valid(cfg);
}

#endif /* OBSERVORE_HOST_TEST */
