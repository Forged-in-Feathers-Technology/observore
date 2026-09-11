#include "argus_led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Active low: driving the pin high turns the LED off. */
#define LED_GPIO    ((gpio_num_t)CONFIG_ARGUS_LED_GPIO)
#define LED_ON()    gpio_set_level(LED_GPIO, 0)
#define LED_OFF()   gpio_set_level(LED_GPIO, 1)

static volatile argus_level_t s_level = ARGUS_LEVEL_CLEAR;
static volatile bool          s_console;

/* One blink period, expressed as on-time and off-time.  Distinguishable at a
 * glance without having to count flashes:
 *   clear   - a short wink every five seconds (proof it is alive)
 *   caution - a steady one-second pulse
 *   alert   - urgent fluttering
 *   console - solid on
 */
static void pattern_for(argus_level_t level, uint32_t *on_ms, uint32_t *off_ms)
{
    switch (level) {
        case ARGUS_LEVEL_ALERT:
            *on_ms = 80;  *off_ms = 120;  break;
        case ARGUS_LEVEL_CAUTION:
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
            LED_ON();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t on_ms, off_ms;
        pattern_for(s_level, &on_ms, &off_ms);

        LED_ON();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        LED_OFF();

        /* Sleep the off-phase in slices so a level change is reflected within
         * a couple of hundred milliseconds rather than at the end of a
         * five-second clear-state gap. */
        argus_level_t at_start = s_level;
        uint32_t slept = 0;
        while (slept < off_ms && s_level == at_start && !s_console) {
            uint32_t slice = (off_ms - slept > 200) ? 200 : (off_ms - slept);
            vTaskDelay(pdMS_TO_TICKS(slice));
            slept += slice;
        }
    }
}

void argus_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    LED_OFF();
    xTaskCreate(led_task, "argus_led", 2048, NULL, 2, NULL);
}

void argus_led_set_level(argus_level_t level)
{
    s_level = level;
}

void argus_led_set_console(bool console)
{
    s_console = console;
}
