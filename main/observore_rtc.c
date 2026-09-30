#include "observore_rtc.h"

#include <stdint.h>

uint8_t observore_bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

uint8_t observore_dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

/* Days from 1970-01-01 for a civil date, by shifting the year to start in
 * March so that the leap day lands at the end of it and the month-length
 * pattern becomes regular. Exact for every date this device will see. */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* 0..399 */
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* 0..146096 */
    return (long)era * 146097L + (long)doe - 719468L;
}

time_t observore_timegm(const struct tm *tm)
{
    if (!tm) {
        return (time_t)-1;
    }
    long days = days_from_civil(tm->tm_year + 1900, (unsigned)(tm->tm_mon + 1),
                               (unsigned)tm->tm_mday);
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L +
                    tm->tm_sec);
}

#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#endif

#if CONFIG_OBSERVORE_RTC

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"

#include "observore_i2c.h"

static const char *TAG = "observore.rtc";

/* The time registers start at 0x04 and run for seven bytes: seconds,
 * minutes, hours, day, weekday, month, year. All BCD. The top bit of the
 * seconds register is the chip saying its own contents are unreliable --
 * set after it loses power, and the one flag worth reading before the time. */
#define RTC_REG_TIME 0x04
#define RTC_VL_BIT   0x80

static i2c_master_dev_handle_t s_dev;

void observore_rtc_init(void)
{
    s_dev = observore_i2c_device(CONFIG_OBSERVORE_RTC_I2C_ADDR, 300 * 1000);
    if (!s_dev) {
        return;
    }
    uint8_t probe = 0;
    if (!observore_i2c_read(s_dev, RTC_REG_TIME, &probe, 1)) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02X", CONFIG_OBSERVORE_RTC_I2C_ADDR);
        s_dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "PCF85063 at 0x%02X%s", CONFIG_OBSERVORE_RTC_I2C_ADDR,
             (probe & RTC_VL_BIT) ? " (contents unreliable, needs setting)" : "");
}

bool observore_rtc_available(void)
{
    return s_dev != NULL;
}

bool observore_rtc_read(void)
{
    uint8_t b[7];
    if (!s_dev || !observore_i2c_read(s_dev, RTC_REG_TIME, b, sizeof(b))) {
        return false;
    }
    if (b[0] & RTC_VL_BIT) {
        ESP_LOGW(TAG, "the chip says its time is unreliable; not using it");
        return false;
    }
    struct tm tm = {
        .tm_sec  = observore_bcd_to_dec(b[0] & 0x7F),
        .tm_min  = observore_bcd_to_dec(b[1] & 0x7F),
        .tm_hour = observore_bcd_to_dec(b[2] & 0x3F),
        .tm_mday = observore_bcd_to_dec(b[3] & 0x3F),
        .tm_mon  = (int)observore_bcd_to_dec(b[5] & 0x1F) - 1,
        /* Two digits, and the chip is younger than the century it is used in. */
        .tm_year = (int)observore_bcd_to_dec(b[6]) + 100,
    };
    /* The chip keeps UTC, because everything stored here does; the timezone
     * is applied when a person reads it, not when it is written down. */
    time_t t = observore_timegm(&tm);
    if (t <= 0) {
        return false;
    }
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "time from the chip: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

bool observore_rtc_write(void)
{
    if (!s_dev) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    uint8_t b[8] = {
        RTC_REG_TIME,
        observore_dec_to_bcd((uint8_t)tm.tm_sec),
        observore_dec_to_bcd((uint8_t)tm.tm_min),
        observore_dec_to_bcd((uint8_t)tm.tm_hour),
        observore_dec_to_bcd((uint8_t)tm.tm_mday),
        observore_dec_to_bcd((uint8_t)tm.tm_wday),
        observore_dec_to_bcd((uint8_t)(tm.tm_mon + 1)),
        observore_dec_to_bcd((uint8_t)(tm.tm_year % 100)),
    };
    if (!observore_i2c_write_bytes(s_dev, b, sizeof(b))) {
        return false;
    }
    ESP_LOGI(TAG, "chip set from the network: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

#else

void observore_rtc_init(void) {}
bool observore_rtc_available(void) { return false; }
bool observore_rtc_read(void) { return false; }
bool observore_rtc_write(void) { return false; }

#endif
