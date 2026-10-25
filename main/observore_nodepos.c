#include "observore_nodepos.h"

#include <string.h>

#include "esp_log.h"
#include "observore_nvs.h"
#include "observore_peer.h"

static const char *TAG = "observore.nodepos";

/* Two signed thirty-two bit values, little-endian, as the chip stores them.
 * A blob rather than two keys so that a half-written pair cannot exist: a
 * latitude without its longitude is not a position, and NVS commits a blob
 * whole. */
typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
} nodepos_blob_t;

static nodepos_blob_t s_pos;
static bool           s_have;

void observore_nodepos_init(void)
{
    nodepos_blob_t b = {0};
    observore_nvs_item_t item = {
        .key = "nodepos", .type = OBSERVORE_NVS_BLOB,
        .buf = &b, .len = sizeof(b),
    };
    if (observore_nvs_read(&item, 1) != ESP_OK || !item.found) {
        return;
    }
    /* Checked on the way out of storage as well as on the way in. The stored
     * value was validated when it was set, but a build that changed the
     * blob's shape would otherwise read the old bytes as a position
     * somewhere in the ocean and start broadcasting it. */
    if (!observore_peer_position_usable(b.lat_e7, b.lon_e7)) {
        ESP_LOGW(TAG, "stored position is not usable; ignoring it");
        return;
    }
    s_pos  = b;
    s_have = true;
    ESP_LOGI(TAG, "this node is at %ld.%07ld, %ld.%07ld, and says so in its "
                  "warnings",
             (long)(b.lat_e7 / 10000000), (long)labs(b.lat_e7 % 10000000),
             (long)(b.lon_e7 / 10000000), (long)labs(b.lon_e7 % 10000000));
}

bool observore_nodepos_get(int32_t *lat_e7, int32_t *lon_e7)
{
    if (!s_have) {
        return false;
    }
    if (lat_e7) { *lat_e7 = s_pos.lat_e7; }
    if (lon_e7) { *lon_e7 = s_pos.lon_e7; }
    return true;
}

bool observore_nodepos_set(int32_t lat_e7, int32_t lon_e7)
{
    if (!observore_peer_position_usable(lat_e7, lon_e7)) {
        return false;
    }
    nodepos_blob_t b = { .lat_e7 = lat_e7, .lon_e7 = lon_e7 };
    observore_nvs_item_t item = {
        .key = "nodepos", .type = OBSERVORE_NVS_BLOB,
        .buf = &b, .len = sizeof(b),
    };
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        return false;
    }
    s_pos  = b;
    s_have = true;
    return true;
}

void observore_nodepos_clear(void)
{
    nodepos_blob_t zero = {0};
    observore_nvs_item_t item = {
        .key = "nodepos", .type = OBSERVORE_NVS_BLOB,
        .buf = &zero, .len = sizeof(zero),
    };
    /* Written as 0,0 rather than deleted, because that is the one coordinate
     * pair this format already refuses -- so a stored zero and an absent key
     * mean the same thing to every reader, including an older build. */
    observore_nvs_write(&item, 1);
    s_have = false;
    memset(&s_pos, 0, sizeof(s_pos));
}
