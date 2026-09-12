#include "observore_clock.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "observore.clock";

/* Any plausible "now" is far past this; the epoch that an unsynced ESP reports
 * is not.  Comparing against a fixed instant rather than tracking a flag also
 * survives a clock set by something other than this module. */
#define SANE_EPOCH 1735689600L   /* 2025-01-01T00:00:00Z */

static int64_t s_synced_at_us;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    bool first = s_synced_at_us == 0;
    s_synced_at_us = esp_timer_get_time();

    char when[32];
    if (observore_clock_iso(s_synced_at_us, when, sizeof(when))) {
        ESP_LOGI(TAG, "%s to %s", first ? "clock set" : "clock resynced", when);
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
    return time(NULL) >= SANE_EPOCH;
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
