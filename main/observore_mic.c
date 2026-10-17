#include "observore_mic.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "observore_nvs.h"
#include "observore_audio.h"

static const char *TAG = "observore.mic";

#if CONFIG_OBSERVORE_MIC

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "observore_ultrasound.h"

static i2s_chan_handle_t s_rx;
static bool     s_ready;
static bool     s_enabled;
static int      s_last_hz;
static int64_t  s_last_us;
static uint32_t s_count;

/* One block's worth of raw frames.
 *
 * The microphone is a 24-bit part in 32-bit slots, which is what almost
 * every I2S MEMS microphone does, so a frame is four bytes and the sample
 * sits in the top three. Taken down to the 16 bits the analyser works in by
 * shifting, not by truncating: the low bits are the quiet ones.
 *
 * This buffer and the int16 one below are the only places audio exists. They
 * are overwritten every 20 ms and nothing copies out of them. */
static int32_t s_raw[OBSERVORE_US_BLOCK];
static int16_t s_mono[OBSERVORE_US_BLOCK];
static observore_us_t s_us;

static void mic_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_enabled) {
            /* Idle cheaply and forget what was learned: a floor trained in
             * one room is wrong in the next, and the gap in between is
             * exactly when the device was carried. */
            observore_us_reset(&s_us);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t got = 0;
        if (i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &got,
                             pdMS_TO_TICKS(500)) != ESP_OK) {
            continue;
        }
        size_t frames = got / sizeof(int32_t);
        if (frames < OBSERVORE_US_BLOCK / 2) {
            continue;
        }
        for (size_t i = 0; i < frames; i++) {
            s_mono[i] = (int16_t)(s_raw[i] >> 16);
        }

        int bin = observore_us_feed(&s_us, s_mono, frames);

        /* A periodic line while listening, because a microphone that is not
         * wired and a room with no beacons in it produce exactly the same
         * output otherwise: nothing. This says which. */
        static int64_t s_said_us;
        int64_t now = esp_timer_get_time();
        if (now - s_said_us > 10 * 1000000) {
            s_said_us = now;
            float peak = 0;
            int at = 0;
            for (int b = 0; b < OBSERVORE_US_BINS; b++) {
                if (s_us.power[b] > peak) {
                    peak = s_us.power[b];
                    at = b;
                }
            }
            ESP_LOGI(TAG, "listening: loudest band %d Hz at %.2e, floor "
                          "%.2e (needs %.0fx to count)",
                     observore_us_bin_hz(at), (double)peak,
                     (double)s_us.floor_[at], (double)OBSERVORE_US_RATIO);
        }

        /* Whatever happened, the audio goes now rather than at the next
         * read. A block that lingers is a block somebody could ask for. */
        memset(s_raw, 0, sizeof(s_raw));
        memset(s_mono, 0, sizeof(s_mono));

        if (bin >= 0) {
            s_last_hz = observore_us_bin_hz(bin);
            s_last_us = esp_timer_get_time();
            s_count++;
            ESP_LOGW(TAG, "ultrasonic tone at %d Hz, held %d ms -- this is "
                          "what cross-device tracking beacons look like",
                     s_last_hz, OBSERVORE_US_RUNS * 1000 * OBSERVORE_US_BLOCK
                                / OBSERVORE_US_RATE);
        }
    }
}

void observore_mic_init(void)
{
    uint8_t on = 0;
    observore_nvs_item_t item = {.key = "mic", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &on, .len = sizeof(on)};
    if (observore_nvs_read(&item, 1) == ESP_OK && item.found &&
        item.len == sizeof(on)) {
        s_enabled = on != 0;
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
                                                        I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan, NULL, &s_rx) != ESP_OK) {
        ESP_LOGW(TAG, "no I2S channel for the microphone");
        return;
    }
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(OBSERVORE_US_RATE),
        /* One channel of the two, because the part is mono and a stereo slot
         * config would hand us its silent half every other frame. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_OBSERVORE_MIC_SCK,
            .ws   = CONFIG_OBSERVORE_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_OBSERVORE_MIC_SD,
            .invert_flags = {0},
        },
    };
    if (i2s_channel_init_std_mode(s_rx, &std) != ESP_OK ||
        i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGW(TAG, "the microphone's I2S would not start");
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return;
    }
    s_ready = true;
    observore_us_reset(&s_us);

    /* 3 KB is enough: the buffers are static and the analyser holds twenty
     * floats. The arithmetic is float, so the task needs the FPU context,
     * which FreeRTOS on the S3 gives any task that uses it. */
    xTaskCreate(mic_task, "observore_mic", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "microphone on I2S sck %d / ws %d / sd %d at %d Hz, "
                  "listening is %s; %d bands from %d to %d Hz, nothing "
                  "below %d Hz is ever computed",
             CONFIG_OBSERVORE_MIC_SCK, CONFIG_OBSERVORE_MIC_WS,
             CONFIG_OBSERVORE_MIC_SD, OBSERVORE_US_RATE,
             s_enabled ? "on" : "off", OBSERVORE_US_BINS,
             observore_us_bin_hz(0),
             observore_us_bin_hz(OBSERVORE_US_BINS - 1),
             observore_us_bin_hz(0));
}

bool observore_mic_enabled(void) { return s_ready && s_enabled; }

void observore_mic_set_enabled(bool on)
{
    s_enabled = on;
    observore_us_reset(&s_us);
    uint8_t v = on ? 1 : 0;
    observore_nvs_item_t item = {.key = "mic", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &v, .len = sizeof(v)};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not remember the microphone setting");
    }
    ESP_LOGW(TAG, "microphone listening %s", on ? "ON" : "off");
}

bool observore_mic_last(int *hz, int64_t *seconds_ago)
{
    if (!s_last_us) {
        return false;
    }
    if (hz) {
        *hz = s_last_hz;
    }
    if (seconds_ago) {
        *seconds_ago = (esp_timer_get_time() - s_last_us) / 1000000;
    }
    return true;
}

uint32_t observore_mic_count(void) { return s_count; }

int observore_mic_bands(float *power, float *floor_, int n)
{
    if (n > OBSERVORE_US_BINS) {
        n = OBSERVORE_US_BINS;
    }
    for (int b = 0; b < n; b++) {
        if (power)  power[b]  = s_us.power[b];
        if (floor_) floor_[b] = s_us.floor_[b];
    }
    return OBSERVORE_US_BINS;
}

int observore_mic_selftest(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "selftest: no microphone");
        return 0;
    }

    /* Across the band, skipping the ends: a tone in the first or last bin
     * has only one neighbour to be confused with and makes a weaker test. */
    static const int TRY[] = {17500, 18000, 18500, 19000, 19500, 20000,
                              20500, 21000};
    /* Of 32767, past the 2700 ceiling an alert may use, because that
     * ceiling protects ears and these tones are above most people's
     * hearing.
     *
     * A build option because it is a trade with no single right answer. Too
     * low and the speaker's poor response this far up means nothing reaches
     * the microphone and the test proves nothing. Too high and the
     * amplifier distorts: at 12000 a 17.5 kHz tone produced a response at
     * 18.75 kHz ten times larger than at its own frequency, which is a
     * distortion product rather than a signal -- and it was audible in the
     * room as a low hum, from tones that are individually inaudible. */
    const int SELFTEST_PEAK = CONFIG_OBSERVORE_MIC_SELFTEST_PEAK;
    const int N_TRY = (int)(sizeof(TRY) / sizeof(TRY[0]));
    const bool was = s_enabled;
    s_enabled = false;          /* stop the task touching the channel */
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGW(TAG, "selftest: sweeping %d tones through the speaker and "
                  "listening for each -- a few seconds, above most people's "
                  "hearing rather than guaranteed silent", N_TRY);

    int heard = 0, placed = 0;
    for (int t = 0; t < N_TRY; t++) {
        int want_bin = (TRY[t] - OBSERVORE_US_BASE_HZ) / OBSERVORE_US_STEP_HZ;

        /* A quiet block first, so the bank's floor is this room rather than
         * whatever the previous tone left behind. */
        observore_us_reset(&s_us);
        size_t got = 0;
        i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &got, pdMS_TO_TICKS(300));
        for (size_t i = 0; i < got / sizeof(int32_t); i++) {
            s_mono[i] = (int16_t)(s_raw[i] >> 16);
        }
        observore_us_feed(&s_us, s_mono, got / sizeof(int32_t));

        /* Then the tone -- played and listened to at the same time.
         *
         * The first version of this called the blocking emitter and then
         * started reading, which does not work: the write returns once the
         * DMA ring has swallowed the samples, so the tone had essentially
         * finished before the first block was captured. It reported 0 of 8
         * and blamed the speaker's high-frequency response. The microphone
         * was fine and the speaker may have been too; nobody was playing
         * anything at the moment anybody listened.
         *
         * Queueing a chunk and then reading a block interleaves them: the
         * ring is still draining that chunk while the block is captured. */
        /* Driven hard, and allowed to be: see observore_audio_tone_start.
         * Nothing here is audible, and the speaker's response this far up is
         * poor enough that the polite level reached the microphone at about
         * 1e-9 -- three orders below the threshold, which is a measurement
         * of the driver rather than of the detector. */
        observore_audio_tone_start(TRY[t], SELFTEST_PEAK);
        observore_audio_tone_chunk(60);      /* prime the ring */
        float best = 0, at_want = 0;
        int best_bin = -1;
        for (int blk = 0; blk < 6; blk++) {
            observore_audio_tone_chunk(30);
            got = 0;
            if (i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &got,
                                 pdMS_TO_TICKS(300)) != ESP_OK) {
                continue;
            }
            size_t frames = got / sizeof(int32_t);
            for (size_t i = 0; i < frames; i++) {
                s_mono[i] = (int16_t)(s_raw[i] >> 16);
            }
            observore_us_feed(&s_us, s_mono, frames);
            for (int b = 0; b < OBSERVORE_US_BINS; b++) {
                if (s_us.power[b] > best) {
                    best = s_us.power[b];
                    best_bin = b;
                }
            }
            if (want_bin >= 0 && want_bin < OBSERVORE_US_BINS &&
                s_us.power[want_bin] > at_want) {
                at_want = s_us.power[want_bin];
            }
        }

        observore_audio_tone_stop();

        /* Two different questions, reported separately because they fail
         * for different reasons and conflating them hides which happened.
         *
         * Landing in the right band says the bank is sharp and the chain
         * carries the frequency it was given. Clearing the threshold says
         * the sound arrived loudly enough to count as a beacon -- which on
         * this board is mostly a statement about a 20x30 mm driver being
         * asked to work two octaves above where it was designed to. */
        /* The criterion the detector itself uses, which is not "this band
         * was the loudest".
         *
         * observore_us_feed fires a band that stands above its own floor and
         * above the absolute threshold; it does not care whether some other
         * band is louder, because a room may contain more than one source.
         * Demanding the loudest band here was a stricter rule than the thing
         * being tested, and it marked a tone that arrived at 5.72e-06 -- six
         * times over the threshold, in exactly the right band -- as a
         * failure, because a louder artifact landed elsewhere.
         *
         * A test that is harder to pass than the system it tests reports
         * failures the system would not have had. */
        bool would_fire = at_want > OBSERVORE_US_MIN_POWER;
        bool loudest    = best_bin == want_bin;
        if (would_fire) {
            heard++;
        }
        if (loudest) {
            placed++;
        }
        ESP_LOGW(TAG, "  %5d Hz -> loudest band %d Hz at %.2e (wanted band "
                      "got %.2e, threshold %.0e)  %s",
                 TRY[t], best_bin >= 0 ? observore_us_bin_hz(best_bin) : 0,
                 (double)best, (double)at_want,
                 (double)OBSERVORE_US_MIN_POWER,
                 would_fire ? (loudest ? "HEARD" : "HEARD (louder artifact "
                                                   "elsewhere)")
                            : (loudest ? "right band, below threshold"
                                       : "no"));
    }

    memset(s_raw, 0, sizeof(s_raw));
    memset(s_mono, 0, sizeof(s_mono));
    observore_us_reset(&s_us);
    s_enabled = was;

    /* What the numbers mean, said here so a bad score is not mistaken for a
     * broken detector. One tone making it all the way through proves the
     * chain: DAC, amplifier, speaker, air, microphone, filter bank. The
     * tones that do not are a statement about this speaker's response two
     * octaves above its comfortable range, and a real beacon is emitted by
     * something built to emit it. */
    ESP_LOGW(TAG, "selftest: %d of %d tones would have fired the detector "
                  "in their own band (%d were also the loudest band). Any "
                  "at all proves the whole chain -- DAC, amplifier, "
                  "speaker, air, microphone, filter bank. The rest measures "
                  "a 20x30 mm driver asked to work at 20 kHz.",
             heard, N_TRY, placed);
    return heard;
}

float observore_mic_peak(void)
{
    float peak = 0;
    for (int b = 0; b < OBSERVORE_US_BINS; b++) {
        if (s_us.power[b] > peak) {
            peak = s_us.power[b];
        }
    }
    return peak;
}

#else  /* no microphone on this board */

void observore_mic_init(void) {}
bool observore_mic_enabled(void) { return false; }
void observore_mic_set_enabled(bool on) { (void)on; }
bool observore_mic_last(int *hz, int64_t *ago) { (void)hz; (void)ago; return false; }
uint32_t observore_mic_count(void) { return 0; }
int observore_mic_bands(float *p, float *f, int n)
{
    (void)p; (void)f; (void)n; return 0;
}
float observore_mic_peak(void) { return 0; }
int observore_mic_selftest(void) { return 0; }

#endif
