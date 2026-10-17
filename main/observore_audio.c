#include "observore_audio.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "observore_nvs.h"

static const char *TAG = "observore.audio";

#if CONFIG_OBSERVORE_AUDIO

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 16 kHz, 16-bit, stereo.
 *
 * Stereo rather than mono because the PCM5101 is a two-channel part and takes
 * its frame from the word-select line; feeding it mono leaves one channel
 * holding whatever was last there. The same sample goes to both.
 *
 * 16 kHz is plenty for a chirp and keeps the buffers small. This is the
 * output path only -- the microphone's sample rate is a different question,
 * because an ultrasonic beacon sits above anything this rate can represent. */
#define RATE 16000

/* Volume, and why the ceiling is so far below full scale.
 *
 * This drives a 2-watt amplifier into an 8-ohm speaker. Full-scale I2S into
 * that is extremely loud in a room, and the first attempt here used a fifth
 * of full scale on the reasoning that a fifth sounds modest. It is not: the
 * number that matters is the acoustic output of the amplifier, not the
 * fraction of the sample range, and 21% of full scale was loud enough to
 * make somebody pull the lead out.
 *
 * So the ceiling is a twelfth of full scale even at volume 100, and the
 * default volume is a fifth of that again. A detector exists to be
 * inconspicuous; the loudest it should ever be is "audible in the next
 * room", and it starts well below that. */
#define AMPLITUDE_MAX 2700            /* of 32767, at volume 100 */
#define VOLUME_MIN    1
#define VOLUME_MAX    100

static i2s_chan_handle_t s_tx;
static bool s_ready;
static bool s_enabled;
static uint8_t s_volume = CONFIG_OBSERVORE_AUDIO_VOLUME;

static int amplitude(void)
{
    int v = s_volume;
    if (v < VOLUME_MIN) v = VOLUME_MIN;
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    return AMPLITUDE_MAX * v / VOLUME_MAX;
}

/* One tone, written straight to the channel.
 *
 * Generated a block at a time rather than into one big buffer: a second of
 * stereo 16-bit at 16 kHz is 64 KB, which is twice the internal heap this
 * board has spare during a patrol window. */
static void tone(int hz, int ms)
{
    if (!s_ready) {
        return;
    }
    enum { BLOCK = 256 };                 /* frames */
    static int16_t buf[BLOCK * 2];
    const int frames = RATE * ms / 1000;
    const double step = 2.0 * M_PI * hz / RATE;
    const int amp = amplitude();
    double phase = 0;

    for (int done = 0; done < frames; done += BLOCK) {
        int n = frames - done < BLOCK ? frames - done : BLOCK;
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(amp * sin(phase));
            phase += step;
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
        }
        size_t wrote = 0;
        if (i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t),
                              &wrote, pdMS_TO_TICKS(200)) != ESP_OK) {
            return;
        }
    }
}

/* Silence between tones, as written zeros rather than a delay.
 *
 * A gap made with vTaskDelay leaves the DMA ring repeating its last buffer,
 * which on this part is an audible buzz rather than a rest. */
static void rest(int ms)
{
    if (!s_ready) {
        return;
    }
    enum { BLOCK = 256 };
    static int16_t quiet[BLOCK * 2];
    const int frames = RATE * ms / 1000;
    for (int done = 0; done < frames; done += BLOCK) {
        int n = frames - done < BLOCK ? frames - done : BLOCK;
        size_t wrote = 0;
        if (i2s_channel_write(s_tx, quiet, (size_t)n * 2 * sizeof(int16_t),
                              &wrote, pdMS_TO_TICKS(200)) != ESP_OK) {
            return;
        }
    }
}

void observore_audio_init(void)
{
    /* On/off and volume in one blob: two settings that are always changed
     * together and never independently useful. */
    struct { uint8_t on, volume; } cfg = {0, CONFIG_OBSERVORE_AUDIO_VOLUME};
    observore_nvs_item_t item = {.key = "audio", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &cfg, .len = sizeof(cfg)};
    if (observore_nvs_read(&item, 1) == ESP_OK && item.found &&
        item.len == sizeof(cfg)) {
        s_enabled = cfg.on != 0;
        if (cfg.volume >= VOLUME_MIN && cfg.volume <= VOLUME_MAX) {
            s_volume = cfg.volume;
        }
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
                                                        I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no I2S channel for the speaker");
        return;
    }
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,     /* the PCM5101 makes its own */
            .bclk = CONFIG_OBSERVORE_AUDIO_BCK,
            .ws   = CONFIG_OBSERVORE_AUDIO_LRCK,
            .dout = CONFIG_OBSERVORE_AUDIO_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGW(TAG, "the speaker's I2S would not start");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }
    s_ready = true;
    /* Says what it is driving, because there is nothing to probe: the DAC has
     * no I2C address and a speaker that is not plugged into the header looks
     * exactly like one that is. */
    ESP_LOGI(TAG, "speaker on I2S bck %d / lrck %d / din %d, sound is %s",
             CONFIG_OBSERVORE_AUDIO_BCK, CONFIG_OBSERVORE_AUDIO_LRCK,
             CONFIG_OBSERVORE_AUDIO_DIN, s_enabled ? "on" : "off");
    ESP_LOGI(TAG, "volume %d of %d (peak %d of 32767 into a 2W amplifier)",
             s_volume, VOLUME_MAX, amplitude());
}

bool observore_audio_enabled(void) { return s_ready && s_enabled; }

int observore_audio_volume(void) { return s_volume; }

void observore_audio_set(bool on, int volume)
{
    s_enabled = on;
    if (volume >= VOLUME_MIN && volume <= VOLUME_MAX) {
        s_volume = (uint8_t)volume;
    }
    struct { uint8_t on, volume; } cfg = {on ? 1u : 0u, s_volume};
    observore_nvs_item_t item = {.key = "audio", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &cfg, .len = sizeof(cfg)};
    if (observore_nvs_write(&item, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not remember the sound setting");
    }
}

void observore_audio_set_enabled(bool on) { observore_audio_set(on, -1); }

void observore_audio_level(observore_level_t from, observore_level_t to)
{
    if (!observore_audio_enabled() || to <= from) {
        return;
    }
    /* Two notes rising for caution, three for alert. Short, because the point
     * is to make somebody look at the screen, not to be a siren. */
    tone(880, 90);
    rest(60);
    tone(1175, 90);
    if (to >= OBSERVORE_LEVEL_ALERT) {
        rest(60);
        tone(1568, 140);
    }
    rest(40);
}

void observore_audio_selftest(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "selftest: no I2S, nothing to play");
        return;
    }
    ESP_LOGI(TAG, "selftest: two short notes at volume %d -- quiet on "
                  "purpose; turn it up rather than starting loud", s_volume);
    tone(880, 120);  rest(80);
    tone(1175, 120);
    rest(50);
}

#else  /* no speaker on this board */

void observore_audio_init(void) {}
bool observore_audio_enabled(void) { return false; }
void observore_audio_set_enabled(bool on) { (void)on; }
int  observore_audio_volume(void) { return 0; }
void observore_audio_set(bool on, int volume) { (void)on; (void)volume; }
void observore_audio_level(observore_level_t f, observore_level_t t)
{
    (void)f; (void)t;
}
void observore_audio_selftest(void)
{
    ESP_LOGW(TAG, "this board has no speaker");
}

#endif
