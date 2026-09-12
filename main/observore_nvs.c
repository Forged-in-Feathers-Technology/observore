#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "observore_nvs.h"

static const char *TAG = "observore.nvs";

#define LEGACY_NAMESPACE "argus"

/* Every key the old namespace could hold, with whether it is a string or a
 * blob.  Listed explicitly rather than enumerated, so a stray key from some
 * abandoned experiment is not carried forward. */
static const struct {
    const char *key;
    bool        is_blob;
} KEYS[] = {
    {"mutes",      true },
    {"sta_ssid",   false},
    {"sta_pass",   false},
    {"gotify_url", false},
    {"gotify_tok", false},
};

static bool copy_key(nvs_handle_t from, nvs_handle_t to, const char *key,
                     bool is_blob)
{
    /* Ask for the size first; these range from a short SSID to a few kilobytes
     * of mute rules. */
    size_t len = 0;
    esp_err_t err = is_blob ? nvs_get_blob(from, key, NULL, &len)
                            : nvs_get_str(from, key, NULL, &len);
    if (err != ESP_OK || len == 0) {
        return false;
    }

    char *buf = malloc(len);
    if (!buf) {
        ESP_LOGE(TAG, "out of memory migrating %s", key);
        return false;
    }

    err = is_blob ? nvs_get_blob(from, key, buf, &len)
                  : nvs_get_str(from, key, buf, &len);
    if (err == ESP_OK) {
        err = is_blob ? nvs_set_blob(to, key, buf, len)
                      : nvs_set_str(to, key, buf);
    }

    /* Credentials pass through this buffer, so do not leave them on the heap
     * for the next allocation to inherit. */
    memset(buf, 0, len);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not migrate %s: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

void observore_nvs_migrate(void)
{
    nvs_handle_t old_ns;
    if (nvs_open(LEGACY_NAMESPACE, NVS_READWRITE, &old_ns) != ESP_OK) {
        return;   /* nothing from the old name -- the normal case */
    }

    nvs_handle_t new_ns;
    if (nvs_open(OBSERVORE_NVS_NAMESPACE, NVS_READWRITE, &new_ns) != ESP_OK) {
        nvs_close(old_ns);
        return;
    }

    size_t moved = 0;
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        /* Never overwrite a setting made since the rename: whatever is under
         * the new name is more recent by definition. */
        size_t existing = 0;
        esp_err_t present = KEYS[i].is_blob
                                ? nvs_get_blob(new_ns, KEYS[i].key, NULL, &existing)
                                : nvs_get_str(new_ns, KEYS[i].key, NULL, &existing);
        if (present == ESP_OK) {
            continue;
        }
        if (copy_key(old_ns, new_ns, KEYS[i].key, KEYS[i].is_blob)) {
            moved++;
        }
    }

    if (moved > 0 && nvs_commit(new_ns) == ESP_OK) {
        ESP_LOGW(TAG, "migrated %zu settings from the old \"%s\" namespace",
                 moved, LEGACY_NAMESPACE);
        /* Only drop the originals once the new copies are committed, so an
         * interrupted migration retries rather than losing the settings. */
        if (nvs_erase_all(old_ns) == ESP_OK) {
            nvs_commit(old_ns);
        }
    }

    nvs_close(new_ns);
    nvs_close(old_ns);
}
