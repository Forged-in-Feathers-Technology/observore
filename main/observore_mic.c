#include "observore_mic.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "observore_nvs.h"

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

#else  /* no microphone on this board */

void observore_mic_init(void) {}
bool observore_mic_enabled(void) { return false; }
void observore_mic_set_enabled(bool on) { (void)on; }
bool observore_mic_last(int *hz, int64_t *ago) { (void)hz; (void)ago; return false; }
uint32_t observore_mic_count(void) { return 0; }

#endif
