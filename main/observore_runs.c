#include "observore_runs.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "observore_nvs.h"

static const char *TAG = "observore.runs";

/* Every five minutes. Precise enough for a battery that lasts hours, and at
 * about three NVS entries a write it fills a page every few hours, which is a
 * sector erase every day or so per page -- a lifetime measured in centuries
 * against the flash's rating, so it is not the thing that wears out. */
#define WRITE_INTERVAL_US (5LL * 60 * 1000000)

/* The blob as stored. Versioned so a later shape can tell an older one apart
 * instead of reading it as garbage. */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  count;        /* completed runs held */
    uint32_t current_s;    /* the running run's last recorded uptime */
    observore_run_t runs[OBSERVORE_RUNS_MAX]; /* oldest first */
} record_t;

#define RECORD_VERSION 1

static record_t s_rec;
static int64_t  s_last_write_us;

static void save(void)
{
    observore_nvs_item_t item = {
        .key = "runs", .type = OBSERVORE_NVS_BLOB,
        .buf = &s_rec, .len = sizeof(s_rec),
    };
    esp_err_t err = observore_nvs_write(&item, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save the run record");
    }
}

void observore_runs_init(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    record_t stored;
    observore_nvs_item_t item = {
        .key = "runs", .type = OBSERVORE_NVS_BLOB,
        .buf = &stored, .len = sizeof(stored),
    };
    if (observore_nvs_read(&item, 1) == ESP_OK && item.found &&
        stored.version == RECORD_VERSION && stored.count <= OBSERVORE_RUNS_MAX) {
        s_rec = stored;
    }
    s_rec.version = RECORD_VERSION;

    /* The value written last time is how long that run lasted, and this
     * boot's reason is how it ended. A run too short to have been written at
     * all -- under five minutes -- leaves nothing behind, which is right: it
     * was not a run so much as a false start. */
    if (s_rec.current_s > 0) {
        if (s_rec.count == OBSERVORE_RUNS_MAX) {
            memmove(&s_rec.runs[0], &s_rec.runs[1],
                    sizeof(s_rec.runs[0]) * (OBSERVORE_RUNS_MAX - 1));
            s_rec.count--;
        }
        s_rec.runs[s_rec.count].up_s = s_rec.current_s;
        s_rec.runs[s_rec.count].end  = (uint8_t)esp_reset_reason();
        s_rec.count++;
        ESP_LOGI(TAG, "previous run: %lu s, ended by %s",
                 (unsigned long)s_rec.current_s,
                 observore_reset_reason_name(esp_reset_reason()));
    }
    s_rec.current_s = 0;
    save();
    s_last_write_us = esp_timer_get_time();
}

void observore_runs_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_write_us < WRITE_INTERVAL_US) {
        return;
    }
    s_last_write_us = now;
    s_rec.current_s = (uint32_t)(now / 1000000);
    save();
}

size_t observore_runs_list(observore_run_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = s_rec.count; i > 0 && n < cap; i--) {
        out[n++] = s_rec.runs[i - 1];
    }
    return n;
}

/* Why this boot happened, which is the same thing as how the previous run
 * ended. A device found up for seven hours after a night on battery could
 * have crashed or could have run flat, and until this was exposed the two
 * were indistinguishable the next morning. */
const char *observore_reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software";    /* esp_restart(): an update, a reboot */
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt-watchdog";
        case ESP_RST_TASK_WDT:  return "task-watchdog";
        case ESP_RST_WDT:       return "watchdog";    /* incl. the rollback watchdog */
        case ESP_RST_BROWNOUT:  return "brownout";    /* the battery ran out */
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "unknown";
    }
}
