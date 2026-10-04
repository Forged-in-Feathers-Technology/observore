#include "observore_monitors.h"

#ifdef OBSERVORE_HOST_TEST
/* No flash on the host. The rules are the whole of what is worth testing and
 * all of them are pure. */
static void monitors_save(void) {}
static void monitors_load(void) {}
#else
#include "esp_log.h"
#include "observore_nvs.h"
#include "observore_detect.h"
static const char *TAG = "observore.monitors";
static void monitors_save(void);
static void monitors_load(void);
#endif

/* Bit n is set when class n is NOT being reported. See the header for why the
 * off set is what gets stored rather than the on set. */
static uint32_t s_off;

/* Every class this build knows, as a bit mask. Anything outside it in a
 * stored value came from a build that knew more classes than this one. */
#define CLASS_MASK ((OBSERVORE_CLASS_MAX >= 32) ? 0xFFFFFFFFu \
                                                : ((1u << OBSERVORE_CLASS_MAX) - 1u))

bool observore_monitors_can_toggle(observore_class_t cls)
{
    /* Unknown is not a monitor. It is what the device says when no monitor
     * matched, so there is nothing to switch off and switching it off would
     * mean hiding everything the device could not name -- the opposite of
     * what this feature is for. */
    if (cls == OBSERVORE_CLASS_UNKNOWN) {
        return false;
    }
    return cls > OBSERVORE_CLASS_UNKNOWN && cls < OBSERVORE_CLASS_MAX;
}

bool observore_monitors_enabled(observore_class_t cls)
{
    if (!observore_monitors_can_toggle(cls)) {
        return true;        /* unknown, and anything out of range, is reported */
    }
    return (s_off & (1u << (unsigned)cls)) == 0u;
}

bool observore_monitors_set(observore_class_t cls, bool on)
{
    if (!observore_monitors_can_toggle(cls)) {
        return false;
    }
    uint32_t before = s_off;
    if (on) {
        s_off &= ~(1u << (unsigned)cls);
    } else {
        s_off |= (1u << (unsigned)cls);
    }
    if (s_off != before) {
        monitors_save();
    }
    return true;
}

int observore_monitors_off_count(void)
{
    uint32_t v = s_off & CLASS_MASK;
    int n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

uint32_t observore_monitors_off_mask(void)
{
    return s_off & CLASS_MASK;
}

bool observore_monitors_restore(uint32_t off_mask)
{
    /* Bits for classes this build does not have are dropped rather than
     * refused. A downgrade should lose the one setting it cannot represent,
     * not every setting -- and refusing the lot would switch monitors back on
     * silently, which is the safe direction but still a change nobody asked
     * for. */
    uint32_t kept = off_mask & CLASS_MASK;

    /* Unknown can never be off, whatever a stored value says. */
    kept &= ~(1u << (unsigned)OBSERVORE_CLASS_UNKNOWN);

    s_off = kept;
    return kept == off_mask;
}

void observore_monitors_init(void)
{
    s_off = OBSERVORE_MONITORS_ALL_ON;
    monitors_load();
}

#ifndef OBSERVORE_HOST_TEST
static void monitors_load(void)
{
    uint32_t v = OBSERVORE_MONITORS_ALL_ON;
    observore_nvs_item_t item = {.key = "monitors", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &v, .len = sizeof(v)};
    if (observore_nvs_read(&item, 1) != ESP_OK || !item.found) {
        return;             /* nothing stored: everything on */
    }
    if (item.len != sizeof(v)) {
        ESP_LOGW(TAG, "the stored monitor set was %u bytes, not %u -- leaving "
                      "every monitor on", (unsigned)item.len, (unsigned)sizeof(v));
        return;
    }
    if (!observore_monitors_restore(v)) {
        ESP_LOGW(TAG, "the stored monitor set named a class this build does "
                      "not have; the rest of it was kept");
    }
    int off = observore_monitors_off_count();
    if (off > 0) {
        /* Said at startup, every time, at a level that survives a release
         * build. A device that is not looking for something should say so
         * without being asked. */
        ESP_LOGW(TAG, "%d monitor%s switched off:", off, off == 1 ? "" : "s");
        for (int c = 1; c < OBSERVORE_CLASS_MAX; c++) {
            if (!observore_monitors_enabled((observore_class_t)c)) {
                ESP_LOGW(TAG, "    %s", observore_class_name(c));
            }
        }
    }
}

static void monitors_save(void)
{
    uint32_t v = observore_monitors_off_mask();
    const observore_nvs_item_t item = {
        .key = "monitors", .type = OBSERVORE_NVS_BLOB, .buf = &v,
        .len = sizeof(v)};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not save which monitors are on");
    }
}
#endif
