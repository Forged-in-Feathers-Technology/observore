#include "observore_motion.h"

#include <limits.h>

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_MOTION

#include <limits.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "observore_i2c.h"
#include "observore_surroundings.h"

static const char *TAG = "observore.motion";

/* QMI8658, from Waveshare's own driver rather than a datasheet nobody has.
 * WHO_AM_I answers 0x05; CTRL1 sets the interface, CTRL2 the accelerometer's
 * range and rate, CTRL7 switches sensors on; the six bytes at 53 are X, Y
 * and Z, little-endian and signed. */
#define QMI_REG_WHOAMI 0x00
#define QMI_REG_CTRL1  0x02
#define QMI_REG_CTRL2  0x03
#define QMI_REG_CTRL7  0x08
#define QMI_REG_AX_L   0x35
#define QMI_WHOAMI     0x05

/* 8 g at 21 Hz, the low-power rate. Nothing here needs precision: the
 * question is "is this thing being carried", and the gyroscope stays off
 * because it answers a question nobody asked at several times the current. */
#define QMI_CTRL1_CFG  0x60
#define QMI_CTRL2_CFG  ((0x02 << 4) | 0x0D)
#define QMI_CTRL7_ACC  0x01
#define QMI_LSB_PER_G  4096

/* Four samples a second is far more often than a person changes between
 * walking and sitting, and costs one six-byte read. */
#define POLL_MS 250

/* Thresholds, in milli-g away from whatever the resting magnitude is.
 *
 * A board at rest reads about 1000 mg of gravity and wanders by a handful.
 * Carrying it swings tens to hundreds. Forty is comfortably above the noise
 * and below anything a person does, including a careful walk. */
#define MOVE_MG        40
#define STILL_SAMPLES  40   /* ten seconds of quiet before it is at rest   */
#define MOVING_SAMPLES 4    /* one second of movement before it is carried */

/* A journey is movement that lasted, not a nudge: a desk knocked in passing
 * must not make the device claim it has been somewhere. */
#define JOURNEY_MS 15000

/* And movement is not arrival.
 *
 * The first version counted a journey from the accelerometer alone, which is
 * how carrying the board around one house promoted every follower in the
 * building: nothing had gone anywhere, and every device that was in range
 * before was in range after. The sensor was right that it moved. It cannot
 * know whether it went anywhere, and those are not the same question.
 *
 * So the access points decide. They are stationary by definition and the
 * board already scans them every fifteen seconds; if most of the ones from
 * before are still in earshot, this is the same place and the journey does
 * not count, however far the thing was carried around it.
 *
 * A third of the old set surviving is the line. Somewhere genuinely else
 * shares almost nothing; a different floor of one building shares a good
 * deal, and calling that "you went somewhere" would bring the false alarms
 * straight back. */
#define ARRIVED_OVERLAP_PCT 33

/* Or the same access points, all of them much fainter.
 *
 * Membership alone cannot answer "did I go anywhere" in the countryside,
 * where the only access points for half a mile are the ones in your own
 * house and they still reach the outbuildings. A barn down a driveway kept
 * eighty percent of them and the journey did not count -- correct by the
 * rule, wrong about the world.
 *
 * Twelve decibels is roughly four times the distance in open air and far
 * more than a person shifts by turning round or putting the thing on a
 * different shelf. Taken as a median across the access points heard at both
 * ends, so one going quiet behind a tractor cannot carry the answer. */
#define ARRIVED_FADE_DB 12

/* Access points heard longer ago than this are not part of "here": a stale
 * entry from the place just left would make the new place look familiar. */
#define SURROUNDINGS_AGE_US (5 * 60 * 1000000LL)

/* After settling, the scan needs a moment to describe the new place before
 * anyone asks it to. Two patrol scans' worth. */
#define ARRIVAL_SETTLE_US (40 * 1000000LL)

static i2c_master_dev_handle_t s_dev;
static volatile bool     s_moving;
static volatile uint32_t s_journeys;
static volatile bool     s_available;
static volatile int      s_last_overlap = -1;
static volatile int      s_last_faded = INT_MIN;
static volatile bool     s_travelling;

static bool read_accel(int *mg)
{
    uint8_t b[6];
    if (!observore_i2c_read(s_dev, QMI_REG_AX_L, b, sizeof(b))) {
        return false;
    }
    int16_t ax = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    int16_t ay = (int16_t)((uint16_t)b[3] << 8 | b[2]);
    int16_t az = (int16_t)((uint16_t)b[5] << 8 | b[4]);
    /* Magnitude, so the answer does not depend on which way up it is. Integer
     * throughout: this runs four times a second for the life of the device
     * and the question is coarse. */
    int64_t sum = (int64_t)ax * ax + (int64_t)ay * ay + (int64_t)az * az;
    int64_t mag = 0;
    while ((mag + 1) * (mag + 1) <= sum) {
        mag++;
    }
    *mg = (int)(mag * 1000 / QMI_LSB_PER_G);
    return true;
}

static void motion_task(void *arg)
{
    (void)arg;
    int  resting = 1000;          /* slow average of the quiet magnitude */
    int  still_run = 0, moving_run = 0;
    int64_t moving_since_us = 0;
    int64_t arrival_due_us = 0;
    bool counted = false;

    for (;;) {
        int mg = 0;
        if (read_accel(&mg)) {
            int delta = abs(mg - resting);
            if (delta < MOVE_MG) {
                /* Follow the resting value while at rest, so a board left on
                 * its side does not read as permanently moving. */
                resting += (mg - resting) / 8;
                moving_run = 0;
                if (still_run < STILL_SAMPLES) {
                    still_run++;
                }
            } else {
                still_run = 0;
                if (moving_run < MOVING_SAMPLES) {
                    moving_run++;
                }
            }

            int64_t now = esp_timer_get_time();
            if (!s_moving && moving_run >= MOVING_SAMPLES) {
                s_moving = true;
                moving_since_us = now;
                counted = false;
                /* What "here" was, before setting off. */
                observore_surroundings_mark(now, SURROUNDINGS_AGE_US);
                s_travelling = true;
                ESP_LOGI(TAG, "picked up (%u access points in earshot)",
                         (unsigned)observore_surroundings_marked());
            } else if (s_moving && still_run >= STILL_SAMPLES) {
                s_moving = false;
                ESP_LOGI(TAG, "set down after %lld s",
                         (long long)((now - moving_since_us) / 1000000));
                /* Long enough to be a journey; whether it went anywhere is
                 * asked once the scan has had time to describe where we are
                 * now. */
                if (!counted && now - moving_since_us >= JOURNEY_MS * 1000LL) {
                    arrival_due_us = now + ARRIVAL_SETTLE_US;
                }
            }

            /* Did we arrive somewhere, or merely stop? */
            if (arrival_due_us && now >= arrival_due_us && !s_moving) {
                arrival_due_us = 0;
                /* The away window closes here, whichever way the verdict
                 * goes: the evidence is complete either way. */
                s_travelling = false;
                int overlap = observore_surroundings_overlap_pct();
                int faded   = observore_surroundings_faded_db();
                s_last_overlap = overlap;
                s_last_faded   = faded;
                bool went = (overlap >= 0 && overlap <= ARRIVED_OVERLAP_PCT) ||
                            (faded != INT_MIN && faded >= ARRIVED_FADE_DB);
                if (overlap < 0) {
                    /* Nothing to compare against. Saying "you went nowhere"
                     * and saying "you arrived" are both inventions here, and
                     * the quiet one is the safer invention. */
                    ESP_LOGI(TAG, "carried, but no access points to judge by");
                } else if (went) {
                    s_journeys++;
                    counted = true;
                    ESP_LOGI(TAG, "journey %u: somewhere else -- %d%% of the "
                                  "old access points still in earshot, and "
                                  "those %d dB fainter",
                             (unsigned)s_journeys, overlap,
                             faded == INT_MIN ? 0 : faded);
                } else {
                    ESP_LOGI(TAG, "carried, but the same place: %d%% of the "
                                  "access points are the ones from before, "
                                  "and only %d dB fainter",
                             overlap, faded == INT_MIN ? 0 : faded);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void observore_motion_init(void)
{
    s_dev = observore_i2c_device(CONFIG_OBSERVORE_MOTION_I2C_ADDR, 300 * 1000);
    if (!s_dev) {
        return;
    }
    uint8_t who = 0;
    if (!observore_i2c_read(s_dev, QMI_REG_WHOAMI, &who, 1) || who != QMI_WHOAMI) {
        ESP_LOGW(TAG, "no QMI8658 at 0x%02X (read 0x%02X)",
                 CONFIG_OBSERVORE_MOTION_I2C_ADDR, who);
        return;
    }
    observore_i2c_write(s_dev, QMI_REG_CTRL1, QMI_CTRL1_CFG);
    observore_i2c_write(s_dev, QMI_REG_CTRL2, QMI_CTRL2_CFG);
    observore_i2c_write(s_dev, QMI_REG_CTRL7, QMI_CTRL7_ACC);

    if (xTaskCreate(motion_task, "motion", 2560, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the motion task");
        return;
    }
    s_available = true;
    ESP_LOGI(TAG, "QMI8658 at 0x%02X, accelerometer only",
             CONFIG_OBSERVORE_MOTION_I2C_ADDR);
}

bool observore_motion_moving(void)     { return s_moving; }
bool observore_motion_travelling(void) { return s_travelling; }
uint32_t observore_motion_journeys(void) { return s_journeys; }
bool observore_motion_available(void)  { return s_available; }
int observore_motion_last_overlap_pct(void) { return s_last_overlap; }
int observore_motion_last_faded_db(void) { return s_last_faded; }

#else

void observore_motion_init(void) {}
bool observore_motion_moving(void) { return false; }
bool observore_motion_travelling(void) { return false; }
uint32_t observore_motion_journeys(void) { return 0; }
bool observore_motion_available(void) { return false; }
int observore_motion_last_overlap_pct(void) { return -1; }
int observore_motion_last_faded_db(void) { return INT_MIN; }

#endif
