#include "observore_gps.h"

#include <stdlib.h>
#include <string.h>

#ifndef OBSERVORE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/* ------------------------------------------------------------------ */
/* Sentence assembly and checksum -- pure, and host tested             */
/* ------------------------------------------------------------------ */

const char *observore_nmea_feed(observore_nmea_scan_t *s, char c)
{
    if (!s) {
        return NULL;
    }
    if (c == '$') {
        /* A fresh start discards whatever was half-collected. A sentence that
         * was cut off mid-way is not worth trying to salvage, and treating the
         * start marker as authoritative means one dropped byte costs one
         * sentence rather than desynchronising the stream for good. */
        s->len = 0;
        s->buf[s->len++] = c;
        return NULL;
    }
    if (s->len == 0) {
        return NULL;            /* noise before any sentence began */
    }
    if (c == '\r' || c == '\n') {
        s->buf[s->len] = '\0';
        const char *done = s->len > 1 ? s->buf : NULL;
        s->len = 0;
        return done;
    }
    if (s->len >= OBSERVORE_NMEA_MAX - 1) {
        /* Longer than NMEA permits. Counted rather than silently dropped: a
         * stream that is always too long is a baud rate that is wrong, and
         * that reads identically to "no GPS" unless something says otherwise. */
        s->too_long++;
        s->len = 0;
        return NULL;
    }
    s->buf[s->len++] = c;
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

bool observore_nmea_checksum_ok(const char *sentence)
{
    if (!sentence || sentence[0] != '$') {
        return false;
    }
    const char *star = strrchr(sentence, '*');
    if (!star || star == sentence) {
        return false;
    }
    int hi = hexval(star[1]);
    int lo = (star[1] != '\0') ? hexval(star[2]) : -1;
    if (hi < 0 || lo < 0 || star[3] != '\0') {
        return false;
    }
    /* XOR of everything between the $ and the *, which is what NMEA says. */
    uint8_t sum = 0;
    for (const char *p = sentence + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    return sum == (uint8_t)((hi << 4) | lo);
}

/* ------------------------------------------------------------------ */
/* Fields                                                             */
/* ------------------------------------------------------------------ */

/* The nth comma-separated field, copied out. Empty is a legitimate value --
 * a module with no fix sends mostly empty fields -- so an empty field is
 * reported as an empty string rather than as absent. */
static bool field(const char *sentence, int n, char *out, size_t out_len)
{
    const char *p = sentence;
    for (int i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
    }
    size_t k = 0;
    while (*p && *p != ',' && *p != '*' && k + 1 < out_len) {
        out[k++] = *p++;
    }
    out[k] = '\0';
    /* Ran out of room rather than reaching a delimiter: the caller would
     * otherwise parse a truncated number as a smaller one. */
    return (*p == '\0' || *p == ',' || *p == '*');
}

static bool two(const char *s, int *out)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

/* ------------------------------------------------------------------ */
/* Sentences                                                          */
/* ------------------------------------------------------------------ */

static bool apply_rmc(observore_gps_t *g, const char *s)
{
    char status[4];
    if (!field(s, 2, status, sizeof(status))) {
        return false;
    }
    /* 'A' is active, 'V' is void. A module with no fix sends V and empty time
     * and date fields, which is exactly what the bench module does indoors --
     * so this is the common case and not an error. */
    if (status[0] != 'A') {
        g->time_valid = false;
        return true;
    }

    char t[12], d[12];
    if (!field(s, 1, t, sizeof(t)) || !field(s, 9, d, sizeof(d))) {
        return false;
    }
    int hh, mm, ss, dd, mo, yy;
    if (strlen(t) < 6 || strlen(d) < 6) {
        return false;
    }
    if (!two(t, &hh) || !two(t + 2, &mm) || !two(t + 4, &ss)) {
        return false;
    }
    if (!two(d, &dd) || !two(d + 2, &mo) || !two(d + 4, &yy)) {
        return false;
    }
    if (hh > 23 || mm > 59 || ss > 60 || dd < 1 || dd > 31 ||
        mo < 1 || mo > 12) {
        return false;
    }

    /* RMC gives a two-digit year and says nothing about the century.
     *
     * Everything from 00 to 79 is read as 2000 to 2079 and the rest as 1980 to
     * 1999, which is the convention the GPS week-number rollover documents use.
     * The practical effect is that this device refuses nothing a real module
     * will send it this century, and the clock's own sanity window --
     * 2025 to 2100 -- rejects the 1980s anyway if one ever arrives. */
    g->year = (yy < 80) ? 2000 + yy : 1900 + yy;
    g->mon  = mo;
    g->day  = dd;
    g->hour = hh;
    g->min  = mm;
    g->sec  = ss;
    g->time_valid = true;
    return true;
}

/* NMEA carries a coordinate as degrees and decimal *minutes* glued together
 * -- ddmm.mmmm for a latitude, dddmm.mmmm for a longitude -- which is the
 * detail that makes a naive atof() wrong by up to 40% of a degree rather
 * than slightly wrong. 5557.1980 is 55 degrees and 57.198 minutes, which is
 * 55.9533 degrees, not 55.571980.
 *
 * Integer throughout, in degrees times ten million, to match what the mesh
 * format carries and because this chip has no floating point to spare. The
 * intermediate is 64-bit: minutes scaled to ten million reach 5.9e8, and the
 * division by sixty has to happen before it is narrowed.
 *
 * Returns false on anything it cannot read whole, which is what a module
 * with no fix sends -- GGA with empty position fields is normal indoors and
 * is not an error. */
static bool nmea_coord(const char *s, int deg_digits, char hemi,
                       int32_t *out_e7)
{
    int i = 0;
    for (i = 0; s[i] >= '0' && s[i] <= '9'; i++) { }
    /* Degrees and whole minutes must both be present; the fraction need not
     * be, and some modules send more digits of it than others. */
    if (i < deg_digits + 2) {
        return false;
    }
    int32_t deg = 0;
    for (int d = 0; d < deg_digits; d++) {
        deg = deg * 10 + (s[d] - '0');
    }
    int64_t min_e7 = 0;
    for (int d = deg_digits; d < i; d++) {
        min_e7 = min_e7 * 10 + (s[d] - '0');
    }
    min_e7 *= 10000000;
    if (s[i] == '.') {
        int64_t scale = 1000000;
        for (int d = i + 1; s[d] >= '0' && s[d] <= '9'; d++) {
            if (scale > 0) {
                min_e7 += (int64_t)(s[d] - '0') * scale;
                scale /= 10;
            }
        }
    }
    int64_t e7 = (int64_t)deg * 10000000 + min_e7 / 60;
    if (hemi == 'S' || hemi == 'W') {
        e7 = -e7;
    } else if (hemi != 'N' && hemi != 'E') {
        return false;
    }
    if (e7 > 1800000000LL || e7 < -1800000000LL) {
        return false;
    }
    *out_e7 = (int32_t)e7;
    return true;
}

static bool apply_gga(observore_gps_t *g, const char *s)
{
    char q[4], n[4];
    if (field(s, 6, q, sizeof(q)) && q[0] >= '0' && q[0] <= '9') {
        g->fix_quality = (uint8_t)(q[0] - '0');
    } else {
        g->fix_quality = 0;
    }
    if (field(s, 7, n, sizeof(n)) && n[0] != '\0') {
        int v = atoi(n);
        g->satellites = (uint8_t)((v < 0) ? 0 : (v > 255 ? 255 : v));
    }

    /* The position, which this parser read past for several releases: it
     * was written to set a clock, and the coordinates were sitting in the
     * same sentence the whole time (#132).
     *
     * Only with a fix. A module reports quality 0 while it is still
     * searching and may still fill the fields with the last thing it
     * believed, and a stale position broadcast as current is worse than
     * none. */
    char lat[16], ns[4], lon[16], ew[4];
    g->have_pos = false;
    if (g->fix_quality > 0 &&
        field(s, 2, lat, sizeof(lat)) && field(s, 3, ns, sizeof(ns)) &&
        field(s, 4, lon, sizeof(lon)) && field(s, 5, ew, sizeof(ew))) {
        int32_t la = 0, lo = 0;
        if (nmea_coord(lat, 2, ns[0], &la) &&
            nmea_coord(lon, 3, ew[0], &lo)) {
            g->lat_e7   = la;
            g->lon_e7   = lo;
            g->have_pos = true;
        }
    }
    return true;
}

static bool apply_gsv(observore_gps_t *g, const char *s)
{
    char n[4];
    if (field(s, 3, n, sizeof(n)) && n[0] != '\0') {
        int v = atoi(n);
        g->in_view = (uint8_t)((v < 0) ? 0 : (v > 255 ? 255 : v));
    }
    return true;
}

static bool apply_txt(observore_gps_t *g, const char *s)
{
    /* The bench module reports "ANTENNA OK" here, and reports a problem the
     * same way. Worth keeping: an antenna fault and a basement look identical
     * from the satellite count alone, and they want different answers. */
    const char *a = strstr(s, "ANTENNA");
    if (!a) {
        return false;
    }
    g->antenna_seen = true;
    g->antenna_ok   = strstr(a, "OK") != NULL;
    return true;
}

bool observore_gps_apply(observore_gps_t *g, const char *s)
{
    if (!g || !s || s[0] != '$' || strlen(s) < 7) {
        return false;
    }
    /* The talker is the two characters after the $ -- GP, GN, GL, GA -- and is
     * deliberately ignored: a multi-constellation module sends GN and a
     * GPS-only one sends GP, and both mean the same thing to this code. */
    const char *type = s + 3;
    bool used;
    if (strncmp(type, "RMC", 3) == 0) {
        used = apply_rmc(g, s);
    } else if (strncmp(type, "GGA", 3) == 0) {
        used = apply_gga(g, s);
    } else if (strncmp(type, "GSV", 3) == 0) {
        used = apply_gsv(g, s);
    } else if (strncmp(type, "TXT", 3) == 0) {
        used = apply_txt(g, s);
    } else {
        return false;
    }
    if (used) {
        g->sentences++;
    }
    return used;
}

bool observore_gps_utc(const observore_gps_t *g, struct tm *out)
{
    if (!g || !out || !g->time_valid) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    /* The C library counts years from 1900 and months from zero. NMEA does
     * neither, and conflating them is a twelve-month error that looks like a
     * working clock. */
    out->tm_year = g->year - 1900;
    out->tm_mon  = g->mon - 1;
    out->tm_mday = g->day;
    out->tm_hour = g->hour;
    out->tm_min  = g->min;
    out->tm_sec  = g->sec;
    out->tm_isdst = 0;
    return true;
}

#ifndef OBSERVORE_HOST_TEST
#if CONFIG_OBSERVORE_GPS

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "observore.gps";

#define GPS_UART       UART_NUM_1
/* One sentence set arrives a second at 9600 baud, so a kilobyte of ring is
 * several seconds of slack -- enough that a busy main loop cannot lose a
 * sentence, and small enough to be free on a board with PSRAM. */
#define GPS_RING       1024
#define GPS_TASK_STACK 3072

static observore_gps_t      s_state;
static observore_nmea_scan_t s_scan;
static SemaphoreHandle_t    s_lock;

observore_gps_t observore_gps_state(void)
{
    observore_gps_t copy;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = s_state;
        xSemaphoreGive(s_lock);
    } else {
        /* A reader that cannot get the lock reports nothing rather than a
         * half-updated struct: the fields are read together and mean nothing
         * apart. */
        memset(&copy, 0, sizeof(copy));
    }
    return copy;
}

static void gps_task(void *arg)
{
    (void)arg;
    size_t reported = (size_t)-1;
    int64_t next_summary = 0;

    for (;;) {
        uint8_t buf[128];
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(500));
        for (int i = 0; i < n; i++) {
            const char *line = observore_nmea_feed(&s_scan, (char)buf[i]);
            if (!line) {
                continue;
            }
            /* Verified before it is believed. A flipped bit in a date field is
             * a clock set to the wrong century, and this line is shared with
             * whatever else is on the bench. */
            if (!observore_nmea_checksum_ok(line)) {
                if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                    s_state.bad_checksum++;
                    xSemaphoreGive(s_lock);
                }
                continue;
            }
            if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                observore_gps_apply(&s_state, line);
                xSemaphoreGive(s_lock);
            }
        }

        /* A summary rather than a sentence a second, and only while something
         * is changing: a module with no sky sends the same empty sentences
         * forever and a log full of them would bury everything else. */
        int64_t now = esp_timer_get_time();
        if (now >= next_summary) {
            next_summary = now + 60 * 1000000;
            observore_gps_t g = observore_gps_state();
            if (g.time_valid) {
                /* With the position, now that one is read. The summary said
                 * "fix 1" and nothing about where, which is no use to
                 * somebody checking whether the coordinates are right --
                 * and a wrong coordinate reads exactly like a right one
                 * until it is printed next to a place you recognise. */
                if (g.have_pos) {
                    ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02dZ, "
                             "%u satellites, fix %u, at %ld.%07ld, "
                             "%ld.%07ld, %u sentences",
                             g.year, g.mon, g.day, g.hour, g.min, g.sec,
                             g.satellites, g.fix_quality,
                             (long)(g.lat_e7 / 10000000),
                             (long)labs(g.lat_e7 % 10000000),
                             (long)(g.lon_e7 / 10000000),
                             (long)labs(g.lon_e7 % 10000000),
                             (unsigned)g.sentences);
                } else {
                    ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02dZ, "
                             "%u satellites, fix %u, no position yet, "
                             "%u sentences", g.year, g.mon, g.day,
                             g.hour, g.min, g.sec, g.satellites,
                             g.fix_quality, (unsigned)g.sentences);
                }
            } else {
                /* The antenna is named because an antenna fault and a basement
                 * look identical from a satellite count of zero, and they want
                 * different answers. */
                ESP_LOGI(TAG, "no fix: %u in view, antenna %s, %u sentences, "
                         "%u bad checksums", g.in_view,
                         !g.antenna_seen ? "unreported"
                                         : (g.antenna_ok ? "ok" : "FAULT"),
                         (unsigned)g.sentences, (unsigned)g.bad_checksum);
            }
            size_t spare = uxTaskGetStackHighWaterMark(NULL);
            if (spare + 256 < reported) {
                reported = spare;
                ESP_LOGI(TAG, "stack headroom: %u bytes of %d",
                         (unsigned)spare, GPS_TASK_STACK);
            }
        }
    }
}

void observore_gps_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no memory for the gps lock");
        return;
    }
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_OBSERVORE_GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, GPS_RING, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return;
    }
    uart_param_config(GPS_UART, &cfg);
    /* Receive only. The module streams without being asked, so nothing here
     * ever drives a pin -- which is also why an input-only pin would do. */
    uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, CONFIG_OBSERVORE_GPS_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (xTaskCreate(gps_task, "observore_gps", GPS_TASK_STACK, NULL, 4, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "could not start the gps task");
        uart_driver_delete(GPS_UART);
        return;
    }
    ESP_LOGI(TAG, "reading a module on GPIO %d at %d baud",
             CONFIG_OBSERVORE_GPS_RX_GPIO, CONFIG_OBSERVORE_GPS_BAUD);
}

#endif /* CONFIG_OBSERVORE_GPS */
#endif /* OBSERVORE_HOST_TEST */
