#include "observore_clock.h"

#include <time.h>

/* The two pure pieces come first and are built on the host as well: the rule
 * about which source wins is the part worth testing and the part that cannot
 * be tested through the rest of this module, which needs a real
 * settimeofday() and a chip on an I2C bus. */

const char *observore_clock_source_name(observore_clock_source_t src)
{
    switch (src) {
    case OBSERVORE_CLOCK_PERSON:  return "person";
    case OBSERVORE_CLOCK_CHIP:    return "chip";
    case OBSERVORE_CLOCK_NETWORK: return "network";
    default:                      return "none";
    }
}

observore_clock_ruling_t observore_clock_rule(observore_clock_source_t have,
                                              bool have_time,
                                              observore_clock_source_t src,
                                              time_t when)
{
    /* Nothing is not a source. Asking to set the clock from nowhere is a
     * caller bug, and treating it as "clear the clock" would give every
     * caller a way to un-date the device. */
    if (src == OBSERVORE_CLOCK_NONE) {
        return OBSERVORE_CLOCK_OUT_OF_RANGE;
    }
    if (when < OBSERVORE_CLOCK_SANE_FROM || when >= OBSERVORE_CLOCK_SANE_UNTIL) {
        return OBSERVORE_CLOCK_OUT_OF_RANGE;
    }
    /* A worse source does not get to undo a better one -- but only while
     * there is a better one to undo. Both halves matter: the case for the
     * first is a browser tab left open on the console, which offers to set
     * the clock and must not replace an SNTP answer with its own opinion on
     * every reload. The case for the second is a power cycle, which leaves
     * the recorded source behind in a chip that no longer knows the time; a
     * ranking honoured then would refuse every answer forever. */
    if (have > src && have_time) {
        return OBSERVORE_CLOCK_WORSE;
    }
    return OBSERVORE_CLOCK_TAKE;
}

#ifndef OBSERVORE_HOST_TEST

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include <stdlib.h>

#include "esp_netif_sntp.h"

#include "observore_census.h"
#include "observore_rtc.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "observore.clock";

/* Any plausible "now" is far past the floor; the epoch an unsynced ESP
 * reports is not. The ceiling is there because a time set by hand can be
 * wrong in the other direction too -- a browser with its year typed wrong is
 * no more usable than 1970, and a date beyond what the census day number can
 * hold would be stored as a different date entirely. Both ends are refused
 * rather than clamped: a clock the device will not use is honest, and one
 * silently moved to the nearest allowed instant is not. */
#define SANE_EPOCH OBSERVORE_CLOCK_SANE_FROM

static int64_t s_synced_at_us;

/* The source travels in RTC memory, with the clock it describes.
 *
 * System time survives esp_restart -- which here means an update installing
 * itself, or a panic or watchdog -- because ESP-IDF keeps the boot time in
 * RTC slow memory. It survived an EN-pin reset on the bench too. It does not
 * survive a power cycle. A source kept in an ordinary static would be
 * lost on a restart while the clock it describes survived, which would turn a
 * known time into an unknown one across every OTA. A source kept in NVS would
 * do the opposite and outlive the clock it describes, claiming a synced time
 * on a board that has just been plugged in. This memory has exactly the right
 * lifetime, which is the whole reason for using it rather than either.
 *
 * It is not initialised at power-on, so it is read through a magic word --
 * without that, the first boot reports whichever source the previous
 * occupant's bits happen to spell. */
#define SOURCE_MAGIC 0x4F43534Bu   /* "OCSK" */
static RTC_NOINIT_ATTR uint32_t s_source_magic;
static RTC_NOINIT_ATTR uint32_t s_source;

observore_clock_source_t observore_clock_source(void)
{
    if (s_source_magic != SOURCE_MAGIC || s_source > OBSERVORE_CLOCK_NETWORK) {
        return OBSERVORE_CLOCK_NONE;
    }
    return (observore_clock_source_t)s_source;
}

static void note_source(observore_clock_source_t src)
{
    s_source_magic = SOURCE_MAGIC;
    s_source = (uint32_t)src;
}

observore_clock_ruling_t observore_clock_set(time_t when,
                                             observore_clock_source_t src)
{
    observore_clock_source_t have = observore_clock_source();
    observore_clock_ruling_t ruling =
        observore_clock_rule(have, time(NULL) >= SANE_EPOCH, src, when);
    switch (ruling) {
    case OBSERVORE_CLOCK_OUT_OF_RANGE:
        ESP_LOGW(TAG, "refusing a time from the %s: %lld is outside the window "
                      "a running device can be in",
                 observore_clock_source_name(src), (long long)when);
        return ruling;
    case OBSERVORE_CLOCK_WORSE:
        ESP_LOGI(TAG, "keeping the time from the %s over an answer from the %s",
                 observore_clock_source_name(have),
                 observore_clock_source_name(src));
        return ruling;
    case OBSERVORE_CLOCK_TAKE:
        break;
    }

    struct timeval tv = {.tv_sec = when, .tv_usec = 0};
    settimeofday(&tv, NULL);
    note_source(src);
    s_synced_at_us = esp_timer_get_time();

    char iso[32];
    if (observore_clock_iso(s_synced_at_us, iso, sizeof(iso))) {
        ESP_LOGI(TAG, "clock set from the %s to %s",
                 observore_clock_source_name(src), iso);
    }
    /* Straight into the chip where there is one, so a time handed in by a
     * person survives the power cycle that loses it everywhere else. */
    if (src != OBSERVORE_CLOCK_CHIP && observore_rtc_available()) {
        observore_rtc_write();
    }
    return OBSERVORE_CLOCK_TAKE;
}

static void on_sync(struct timeval *tv)
{
    (void)tv;
    /* "Set" against "resynced" is about whether the device already knew the
     * time, not about whether this module is the one that told it. A clock
     * retained across a restart, or seeded from the chip, is already a set
     * clock -- and reporting the first SNTP reply after one as "clock set"
     * reads as though the device had been adrift until then. */
    bool first = !observore_clock_valid();
    s_synced_at_us = esp_timer_get_time();
    note_source(OBSERVORE_CLOCK_NETWORK);

    char when[32];
    if (observore_clock_iso(s_synced_at_us, when, sizeof(when))) {
        ESP_LOGI(TAG, "%s to %s", first ? "clock set" : "clock resynced", when);
    }

    /* Hand it to the chip, which is the half of this pair that survives a
     * reboot away from a network. The two correct each other in the direction
     * that makes sense: the network is more accurate, the chip is more
     * available. */
    if (observore_rtc_available()) {
        observore_rtc_write();
    }
}

void observore_clock_init(void)
{
    /* DHCP first, and that is not a detail.  This device is meant to sit on
     * the segment somebody keeps their cameras on, and those are routinely
     * firewalled from the internet -- the same isolation that stops a push
     * notification reaching its server stops pool.ntp.org answering.  A
     * router's own NTP is reachable from inside that fence; a public pool
     * often is not.  The configured server remains as the fallback for
     * networks that hand out no NTP option. */
    /* The timezone belongs to whoever reads a time, not to the time itself:
     * everything stored stays UTC. This only affects what a person is shown. */
    setenv("TZ", CONFIG_OBSERVORE_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_OBSERVORE_NTP_SERVER);
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.start = true;
    /* Never block: this runs on a device that is only intermittently
     * associated, and a sync that has not happened yet is a reportable state
     * rather than an error to wait on. */
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sync;
    /* Stepping, not slewing.  Smooth sync would take minutes to correct a
     * clock that starts in 1970, and a detector that cannot date anything for
     * the first few minutes after boot is the case this exists to fix. */
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no time synchronisation: %s -- times will be relative "
                      "to boot", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "waiting for the time (DHCP, then %s)",
             CONFIG_OBSERVORE_NTP_SERVER);
}

bool observore_clock_valid(void)
{
    return observore_clock_source() != OBSERVORE_CLOCK_NONE &&
           time(NULL) >= SANE_EPOCH;
}

time_t observore_clock_at(int64_t uptime_us)
{
    if (!observore_clock_valid()) {
        return 0;
    }
    /* Work from the monotonic delta rather than from a stored offset, so the
     * answer stays right across a resync and is correct for timestamps taken
     * before the clock was ever set. */
    int64_t now_us = esp_timer_get_time();
    int64_t ago_us = now_us - uptime_us;
    if (ago_us < 0) {
        ago_us = 0;                  /* a timestamp from the future is a bug */
    }
    return time(NULL) - (time_t)(ago_us / 1000000);
}

bool observore_clock_iso(int64_t uptime_us, char *out, size_t len)
{
    if (!out || len == 0) {
        return false;
    }
    out[0] = '\0';
    time_t t = observore_clock_at(uptime_us);
    if (t == 0) {
        return false;
    }
    struct tm utc;
    gmtime_r(&t, &utc);
    return strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
}

int64_t observore_clock_synced_at(void)
{
    return s_synced_at_us;
}

int observore_clock_day(void)
{
    if (!observore_clock_valid()) {
        return OBSERVORE_CENSUS_NO_DAY;
    }
    return observore_census_day(time(NULL));
}

#endif /* OBSERVORE_HOST_TEST */
