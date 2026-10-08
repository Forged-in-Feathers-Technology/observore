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
    uint16_t current_mv_start;  /* the running run's first cell reading */
    uint16_t current_mv_end;    /* and its most recent one */
    observore_run_t runs[OBSERVORE_RUNS_MAX]; /* oldest first */
} record_t;

#define RECORD_VERSION 2

/* Version 1, which held no voltages, kept so its records can be carried over
 * rather than thrown away.
 *
 * The census discarded its old format on the same kind of change and was
 * right to: there, an old blob with an even number of entries divided evenly
 * into the new entry size and would have restored as half as many entries of
 * garbage. The danger was misreading, not the upgrade. Here the version byte
 * is checked explicitly before either layout is touched, so there is no
 * misreading to risk -- and the runs being carried over are measurements
 * somebody made by leaving a board on a battery overnight, which is not the
 * sort of thing to drop for the sake of nine bytes an entry. */
/* NOT packed, because version 1's entry was not packed either: it was the
 * public observore_run_t of the day, {uint32, uint8}, which the compiler
 * padded to eight bytes inside the packed record. Declaring this one packed
 * made it five, made the record 46 bytes against the 70 actually stored, and
 * the read then failed with ESP_ERR_NVS_INVALID_LENGTH -- which means the
 * buffer was too small, not that the data was wrong. The migration found
 * nothing, and the save that followed overwrote a real run history.
 *
 * The sizes had been measured on the host before this was written -- 8 bytes
 * unpacked, 5 packed, 70 for the record -- and the code was written the other
 * way regardless. A stored layout is whatever the writer's compiler produced,
 * not whatever looks tidy. */
typedef struct {
    uint32_t up_s;
    uint8_t  end;
} run_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  count;
    uint32_t current_s;
    run_v1_t runs[OBSERVORE_RUNS_MAX];
} record_v1_t;

/* The size a device in the field actually has stored. Checked at compile time
 * rather than discovered by a failed read on somebody's board: this is the
 * one number in the file that is not ours to choose. */
_Static_assert(sizeof(record_v1_t) == 70,
               "version 1 records are 70 bytes on every board that wrote one");

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

/* Restore the record, whichever layout is stored.
 *
 * One read into a buffer big enough for any layout this build knows, then the
 * decision from the version byte and the length NVS reports. The first
 * attempt read straight into the current record_t and let the read fail when
 * the size did not match, which produced a warning naming an NVS error code
 * and nothing about what was actually there -- and the error does not mean
 * what it looks like: nvs_get_blob returns ESP_ERR_NVS_INVALID_LENGTH when
 * the buffer is *too small*, so an oversized buffer is fine and a failure
 * says the stored blob is bigger than anything this build expects.
 *
 * So the unreadable case says what it found. A record that cannot be read
 * should report its length and its version rather than an error code; that is
 * the difference between a lead and a dead end. */
static void restore(void)
{
    union {
        record_t    v2;
        record_v1_t v1;
        uint8_t     raw[1];
    } buf;
    _Static_assert(sizeof(record_t) >= sizeof(record_v1_t),
                   "the read buffer must cover every layout this build knows");

    memset(&buf, 0, sizeof(buf));
    observore_nvs_item_t item = {
        .key = "runs", .type = OBSERVORE_NVS_BLOB,
        .buf = &buf, .len = sizeof(buf),
    };
    observore_nvs_read(&item, 1);
    if (!item.found) {
        return;                      /* nothing stored yet: a first boot */
    }

    if (buf.raw[0] == RECORD_VERSION && item.len == sizeof(record_t) &&
        buf.v2.count <= OBSERVORE_RUNS_MAX) {
        s_rec = buf.v2;
        return;
    }

    /* Version 1 held no voltages. Its lengths and endings are still true, so
     * they are carried forward rather than thrown away: those runs are
     * measurements somebody made by leaving a board on a battery overnight.
     *
     * The census discarded its old format on the same kind of change and was
     * right to -- there an old blob divided evenly into the new entry size
     * and would have restored as garbage, so the danger was misreading. Here
     * the version byte and the length are both checked before either layout
     * is touched, so there is no misreading to risk. */
    if (buf.raw[0] == 1 && item.len == sizeof(record_v1_t) &&
        buf.v1.count <= OBSERVORE_RUNS_MAX) {
        record_v1_t old = buf.v1;
        memset(&s_rec, 0, sizeof(s_rec));
        s_rec.count     = old.count;
        s_rec.current_s = old.current_s;
        for (uint8_t i = 0; i < old.count; i++) {
            s_rec.runs[i].up_s = old.runs[i].up_s;
            s_rec.runs[i].end  = old.runs[i].end;
        }
        ESP_LOGI(TAG, "carried %u run%s forward from record format 1",
                 (unsigned)old.count, old.count == 1 ? "" : "s");
        return;
    }

    ESP_LOGW(TAG, "stored run record is %u bytes, version %u -- this build "
                  "knows %u (v%u) and %u (v1); starting fresh",
             (unsigned)item.len, (unsigned)buf.raw[0],
             (unsigned)sizeof(record_t), RECORD_VERSION,
             (unsigned)sizeof(record_v1_t));
    memset(&s_rec, 0, sizeof(s_rec));
}

void observore_runs_init(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    restore();
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
        s_rec.runs[s_rec.count].up_s     = s_rec.current_s;
        s_rec.runs[s_rec.count].end      = (uint8_t)esp_reset_reason();
        s_rec.runs[s_rec.count].mv_start = s_rec.current_mv_start;
        s_rec.runs[s_rec.count].mv_end   = s_rec.current_mv_end;
        s_rec.count++;
        if (s_rec.current_mv_start > 0) {
            /* The span, said at boot, because it is the answer to the
             * question the record exists for and the serial log is where
             * somebody looks after a board has died overnight. */
            ESP_LOGI(TAG, "previous run: %lu s, %u mV to %u mV, ended by %s",
                     (unsigned long)s_rec.current_s,
                     (unsigned)s_rec.current_mv_start,
                     (unsigned)s_rec.current_mv_end,
                     observore_reset_reason_name(esp_reset_reason()));
        } else {
            ESP_LOGI(TAG, "previous run: %lu s, ended by %s",
                     (unsigned long)s_rec.current_s,
                     observore_reset_reason_name(esp_reset_reason()));
        }
    }
    s_rec.current_s = 0;
    s_rec.current_mv_start = 0;
    s_rec.current_mv_end = 0;
    save();
    s_last_write_us = esp_timer_get_time();
}

void observore_runs_note_mv(uint16_t mv)
{
    if (mv == 0) {
        return;              /* no sense on this board, or no reading yet */
    }
    if (s_rec.current_mv_start == 0) {
        s_rec.current_mv_start = mv;
    }
    s_rec.current_mv_end = mv;
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
