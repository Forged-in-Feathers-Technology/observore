#include "observore_sdlog.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "observore.sdlog";

#if CONFIG_OBSERVORE_SDLOG

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_timer.h"

#include "observore_clock.h"
#include "observore_csv.h"
#include "observore_detect.h"
#include "observore_util.h"
#include "observore_sd.h"

/* Eight-dot-three, everywhere, without exception.
 *
 * ESP-IDF's FATFS is built with CONFIG_FATFS_LFN_NONE here, so a longer
 * name is not a tidier name, it is EINVAL from fopen. That cost a round of
 * debugging during bring-up against a message that guessed at "read-only,
 * or full" instead of printing errno. "OBSERVOR" is eight characters
 * because "OBSERVORE" is nine. */
#define LOG_DIR  "/sd/OBSERVOR"

/* One header, written when a file is created, naming what each column is.
 *
 * `when` is UTC and empty until the clock has been set; `uptime_s` is
 * always there. Two columns rather than one that changes meaning, because a
 * reader should not have to guess which kind of timestamp it has -- and a
 * board that has never reached an NTP server still produces an ordered log,
 * which is most of what the timestamps were for.
 *
 * `uptime_s` is seconds since *this* boot, so it restarts within a file
 * whenever the device does. Reading a real day's file makes that obvious
 * and slightly startling -- 3, 6, 2, 2, 4 down the column, because those
 * rows came from four boots. It is correct and it is not an ordering: sort
 * on `when` where there is one, and treat a drop in `uptime_s` as the mark
 * of a restart, which is itself worth knowing. */
#define LOG_HEADER \
    "when,uptime_s,class,mac,random,source,rssi,channel,evidence,label," \
    "detail,vendor,hits\n"

static FILE    *s_file;
static char     s_open_name[16];
static uint32_t s_written;
static uint32_t s_dropped;
static bool     s_ready;

/* Which file this record belongs in. Returns false when it cannot be known
 * yet, which only happens for daily rotation before the clock is set. */
static void current_name(char *out, size_t cap)
{
#if CONFIG_OBSERVORE_SDLOG_ROTATE_DAILY
    /* A day per file. The name is the date, so the card is readable
     * without opening anything. */
    time_t t = observore_clock_at(esp_timer_get_time());
    struct tm tm;
    if (observore_clock_valid() && gmtime_r(&t, &tm)) {
        /* Clamped, which the compiler insists on and which is right
         * anyway: a struct tm that has come from a clock nobody has
         * checked should not be able to name a file of any length it
         * likes. */
        int yy = ((tm.tm_year % 100) + 100) % 100;
        int mm = tm.tm_mon + 1;
        int dd = tm.tm_mday;
        if (mm < 1)  mm = 1;
        if (mm > 12) mm = 12;
        if (dd < 1)  dd = 1;
        if (dd > 31) dd = 31;
        snprintf(out, cap, "%02d%02d%02d.CSV", yy, mm, dd);
        return;
    }
    /* No trusted clock, so no date to name a file after. Undated records
     * still go somewhere rather than being dropped: a detector that stops
     * logging because it does not know the date has failed at the easier
     * of its two jobs. They carry uptime, and the file is named so that
     * nobody mistakes it for a day's worth. */
    snprintf(out, cap, "UNDATED.CSV");
#elif CONFIG_OBSERVORE_SDLOG_ROTATE_SIZE
    /* A new file every so many kilobytes. The counter is found by looking,
     * not remembered, so a reboot continues rather than overwriting. */
    static int seq = -1;
    if (seq < 0) {
        seq = 0;
        for (int i = 0; i < 100000; i++) {
            char probe[32];
            struct stat st;
            snprintf(probe, sizeof(probe), LOG_DIR "/OBS%05d.CSV", i);
            if (stat(probe, &st) != 0) {
                break;
            }
            /* The last file is continued if it still has room. */
            seq = (st.st_size < CONFIG_OBSERVORE_SDLOG_MAX_KB * 1024) ? i : i + 1;
        }
    }
    char probe[32];
    struct stat st;
    snprintf(probe, sizeof(probe), LOG_DIR "/OBS%05d.CSV", seq);
    if (stat(probe, &st) == 0 &&
        st.st_size >= CONFIG_OBSERVORE_SDLOG_MAX_KB * 1024) {
        seq++;
    }
    snprintf(out, cap, "OBS%05d.CSV", seq);
#else
    snprintf(out, cap, "OBSERVOR.CSV");
#endif
}

/* Open the file this record belongs in, creating it and its header if it is
 * new, and closing whatever was open if the name has changed. */
static FILE *file_for_now(void)
{
    char want[16];
    current_name(want, sizeof(want));

    if (s_file && strcmp(want, s_open_name) == 0) {
        return s_file;
    }
    if (s_file) {
        ESP_LOGI(TAG, "rotating from %s to %s", s_open_name, want);
        fclose(s_file);
        s_file = NULL;
    }

    char path[48];
    snprintf(path, sizeof(path), LOG_DIR "/%s", want);

    struct stat st;
    bool fresh = stat(path, &st) != 0;

    s_file = fopen(path, "a");
    if (!s_file) {
        ESP_LOGW(TAG, "cannot append to %s: errno %d (%s)", path, errno,
                 strerror(errno));
        return NULL;
    }
    snprintf(s_open_name, sizeof(s_open_name), "%s", want);
    if (fresh) {
        fputs(LOG_HEADER, s_file);
        ESP_LOGI(TAG, "started %s", path);
    }
    return s_file;
}

void observore_sdlog_init(void)
{
    if (!observore_sd_ready()) {
        return;
    }
    /* mkdir on a directory that exists returns EEXIST, which is not a
     * failure here. */
    if (mkdir(LOG_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "cannot make %s: errno %d (%s); not logging", LOG_DIR,
                 errno, strerror(errno));
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "logging detections to %s as CSV", LOG_DIR);
}

void observore_sdlog_note(const observore_event_t *e)
{
    if (!s_ready || !e) {
        return;
    }
    FILE *f = file_for_now();
    if (!f) {
        s_dropped++;
        return;
    }

    char when[32] = "";
    observore_clock_iso(e->last_seen_us, when, sizeof(when));

    char macbuf[OBSERVORE_MAC_STR_LEN];
    char label[OBSERVORE_LABEL_LEN * 2 + 4];
    char detail[sizeof(e->detail) * 2 + 4];
    char vendor[40];
    observore_csv_field(label, sizeof(label), e->label);
    observore_csv_field(detail, sizeof(detail), e->detail);
    observore_csv_field(vendor, sizeof(vendor), e->vendor);

    int n = fprintf(f, "%s,%lld,%s,%s,%d,%s,%d,%u,%s,%s,%s,%s,%u\n",
                    when, (long long)(e->last_seen_us / 1000000),
                    observore_class_name(e->cls),
                    observore_mac_str(e->mac, macbuf),
                    e->addr_random ? 1 : 0,
                    observore_source_name(e->src),
                    e->rssi, (unsigned)e->channel,
                    observore_evidence_name(e->evidence),
                    label, detail, vendor, (unsigned)e->hits);
    if (n < 0) {
        s_dropped++;
        return;
    }

    /* Flushed to the card for every record, not every so often.
     *
     * This device gets unplugged -- that is the normal way it stops. A
     * buffered log loses whatever was in the buffer, which is precisely the
     * most recent detections and precisely the ones somebody came back to
     * read. The cost is a few milliseconds against detections that arrive
     * seconds apart at the very fastest.
     *
     * A cut during the flush can still truncate the final line, and CSV is
     * the right format for that: a half-written row is visibly half-written
     * and every reader skips it. The same accident in a binary log can
     * corrupt the whole file. */
    fflush(f);
    fsync(fileno(f));
    s_written++;
}

void observore_sdlog_counts(uint32_t *written, uint32_t *dropped)
{
    if (written) *written = s_written;
    if (dropped) *dropped = s_dropped;
}

#else  /* no card logging on this board */

void observore_sdlog_init(void) {}
void observore_sdlog_note(const observore_event_t *e) { (void)e; }
void observore_sdlog_counts(uint32_t *w, uint32_t *d)
{
    if (w) *w = 0;
    if (d) *d = 0;
}

#endif
