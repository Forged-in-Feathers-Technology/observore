#include "observore_meshkey.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/md.h"

#include "observore_nvs.h"

static const char *TAG = "observore.meshkey";

static uint8_t s_key[OBSERVORE_MESHKEY_BYTES];
static bool    s_have;

static int unhex(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static void save(const char *hex)
{
    observore_nvs_item_t item = {
        .key = "meshkey", .type = OBSERVORE_NVS_STR,
        .buf = (void *)(hex ? hex : ""), .len = hex ? strlen(hex) + 1 : 1,
    };
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the mesh key");
    }
}

bool observore_meshkey_set(const char *hex)
{
    if (!hex || hex[0] == '\0') {
        memset(s_key, 0, sizeof(s_key));
        s_have = false;
        save(NULL);
        ESP_LOGI(TAG, "mesh key cleared; warnings will all be from strangers");
        return true;
    }
    if (strlen(hex) != OBSERVORE_MESHKEY_HEX) {
        return false;
    }
    /* Parsed whole before anything is kept. A half-parsed key is a key
     * nobody chose, and it would verify nothing while reporting that a key
     * is present -- which reads as "the mesh is signed" when it is not. */
    uint8_t parsed[OBSERVORE_MESHKEY_BYTES];
    for (size_t i = 0; i < OBSERVORE_MESHKEY_BYTES; i++) {
        int hi = unhex(hex[i * 2]), lo = unhex(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        parsed[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(s_key, parsed, sizeof(s_key));
    s_have = true;
    save(hex);
    /* Never the key, and not even its length beyond what is already fixed.
     * A log is the one place on this device a secret has leaked before. */
    ESP_LOGI(TAG, "mesh key set; tagged warnings from your nodes will verify");
    return true;
}

void observore_meshkey_init(void)
{
    char hex[OBSERVORE_MESHKEY_HEX + 1] = {0};
    observore_nvs_item_t item = {
        .key = "meshkey", .type = OBSERVORE_NVS_STR,
        .buf = hex, .len = sizeof(hex),
    };
    observore_nvs_read(&item, 1);
    if (!item.found || hex[0] == '\0') {
        s_have = false;
        return;
    }
    if (!observore_meshkey_set(hex)) {
        /* Stored and unusable is worth saying: the node will trust nothing
         * and the owner would otherwise have no way to know why. */
        ESP_LOGW(TAG, "the stored mesh key is not %d hex characters; "
                      "trusting no warnings", OBSERVORE_MESHKEY_HEX);
        s_have = false;
    }
}

bool observore_meshkey_present(void)
{
    return s_have;
}

bool observore_meshkey_tag(const uint8_t *msg, size_t len, uint8_t *out)
{
    if (!s_have || !msg || !out || len == 0) {
        return false;
    }
    uint8_t full[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, s_key, sizeof(s_key), msg, len, full) != 0) {
        return false;
    }
    /* The first four bytes. Which four does not matter cryptographically --
     * every byte of an HMAC is equally unpredictable without the key -- and
     * the leading ones are what every other truncated-MAC format takes, so
     * there is nothing to get wrong on the far side. */
    memcpy(out, full, OBSERVORE_PEER_TAG_LEN);
    return true;
}

bool observore_meshkey_verify(const uint8_t *msg, size_t len,
                              const uint8_t *tag)
{
    uint8_t want[OBSERVORE_PEER_TAG_LEN];
    if (!tag || !observore_meshkey_tag(msg, len, want)) {
        return false;      /* no key: a node with none trusts nothing */
    }
    /* Constant time. Four bytes is not much to leak and a timing oracle on a
     * BLE advert is not a realistic attack, but the alternative is writing a
     * memcmp into the one comparison on the device that decides whether a
     * message is trusted, and that is not a line to be casual on. */
    uint8_t diff = 0;
    for (size_t i = 0; i < OBSERVORE_PEER_TAG_LEN; i++) {
        diff |= (uint8_t)(want[i] ^ tag[i]);
    }
    return diff == 0;
}
