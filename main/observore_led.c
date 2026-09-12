#include "observore_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_OBSERVORE_LED_ADDRESSABLE
#include "led_strip.h"
#else
#include "driver/gpio.h"
#endif

static volatile observore_level_t s_level = OBSERVORE_LEVEL_CLEAR;
static volatile bool          s_console;

/* Two boards, two kinds of LED.  Everything above backend_show() is shared:
 * the blink rhythm is the same either way, and only the rendering differs.
 *
 * Colour does not replace the rhythm on boards that have it.  A device meant
 * to sit unattended in a room should not also be a lit beacon announcing
 * itself, so the LED stays dark most of the time on both backends; colour adds
 * a second channel of information rather than changing how visible the thing
 * is. */

#if CONFIG_OBSERVORE_LED_ADDRESSABLE

static led_strip_handle_t s_strip;

static void backend_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = CONFIG_OBSERVORE_LED_GPIO,
        .max_leds         = 1,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    /* RMT rather than SPI: it is available on every target this project
     * builds for, and one pixel refreshed a few times a second is nowhere
     * near needing DMA. */
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags             = {.with_dma = false},
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        s_strip = NULL;   /* carry on headless rather than refusing to boot */
    }
}

static void backend_show(bool lit, observore_level_t level, bool console)
{
    if (!s_strip) {
        return;
    }
    if (!lit) {
        led_strip_clear(s_strip);
        return;
    }

    uint8_t r, g, b;
    if (console) {
        r = 0;   g = 0;   b = 255;   /* blue: the console is up */
    } else {
        switch (level) {
            case OBSERVORE_LEVEL_ALERT:   r = 255; g = 0;   b = 0;   break;
            case OBSERVORE_LEVEL_CAUTION: r = 255; g = 120; b = 0;   break;
            default:                      r = 0;   g = 255; b = 0;   break;
        }
    }

    const uint32_t scale = CONFIG_OBSERVORE_LED_BRIGHTNESS;
    led_strip_set_pixel(s_strip, 0,
                        (uint8_t)(r * scale / 255),
                        (uint8_t)(g * scale / 255),
                        (uint8_t)(b * scale / 255));
    led_strip_refresh(s_strip);
}

#else  /* CONFIG_OBSERVORE_LED_MONO */

#define LED_GPIO ((gpio_num_t)CONFIG_OBSERVORE_LED_GPIO)

static void backend_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void backend_show(bool lit, observore_level_t level, bool console)
{
    (void)level;     /* one colour; the rhythm carries the level */
    (void)console;
#if CONFIG_OBSERVORE_LED_ACTIVE_LOW
    gpio_set_level(LED_GPIO, lit ? 0 : 1);
#else
    gpio_set_level(LED_GPIO, lit ? 1 : 0);
#endif
}

#endif

/* One blink period, expressed as on-time and off-time.  Distinguishable at a
 * glance without having to count flashes:
 *   clear   - a short wink every five seconds (proof it is alive)
 *   caution - a steady one-second pulse
 *   alert   - urgent fluttering
 *   console - solid on
 */
static void pattern_for(observore_level_t level, uint32_t *on_ms, uint32_t *off_ms)
{
    switch (level) {
        case OBSERVORE_LEVEL_ALERT:
            *on_ms = 80;  *off_ms = 120;  break;
        case OBSERVORE_LEVEL_CAUTION:
            *on_ms = 250; *off_ms = 750;  break;
        default:
            *on_ms = 40;  *off_ms = 4960; break;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_console) {
            backend_show(true, s_level, true);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t on_ms, off_ms;
        observore_level_t level = s_level;
        pattern_for(level, &on_ms, &off_ms);

        backend_show(true, level, false);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        backend_show(false, level, false);

        /* Sleep the off-phase in slices so a level change is reflected within
         * a couple of hundred milliseconds rather than at the end of a
         * five-second clear-state gap. */
        uint32_t slept = 0;
        while (slept < off_ms && s_level == level && !s_console) {
            uint32_t slice = (off_ms - slept > 200) ? 200 : (off_ms - slept);
            vTaskDelay(pdMS_TO_TICKS(slice));
            slept += slice;
        }
    }
}

void observore_led_init(void)
{
    backend_init();
    backend_show(false, OBSERVORE_LEVEL_CLEAR, false);
    xTaskCreate(led_task, "observore_led", 2560, NULL, 2, NULL);
}

void observore_led_set_level(observore_level_t level)
{
    s_level = level;
}

void observore_led_set_console(bool console)
{
    s_console = console;
}
