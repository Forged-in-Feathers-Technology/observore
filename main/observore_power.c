#include "observore_power.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "observore.power";

#if CONFIG_OBSERVORE_POWER_HOLD_GPIO >= 0

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* How long the button must be held to switch off.
 *
 * Long enough that it cannot happen by accident in a pocket, short enough
 * that somebody who means it does not conclude the board has hung. Two
 * seconds is what the sibling board in this family documents. */
#define OFF_HOLD_MS 2000

static bool s_held;

void observore_power_hold(void)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << CONFIG_OBSERVORE_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&out) != ESP_OK) {
        ESP_LOGE(TAG, "could not claim the power hold on GPIO%d -- this "
                      "board will not stay on when the button is released",
                 CONFIG_OBSERVORE_POWER_HOLD_GPIO);
        return;
    }
    gpio_set_level(CONFIG_OBSERVORE_POWER_HOLD_GPIO, 1);
    s_held = true;
    ESP_LOGI(TAG, "power latched on (GPIO%d high)",
             CONFIG_OBSERVORE_POWER_HOLD_GPIO);
}

bool observore_power_button(void)
{
#if CONFIG_OBSERVORE_POWER_KEY_GPIO >= 0
    /* Pulled high, grounded by a press. */
    return gpio_get_level(CONFIG_OBSERVORE_POWER_KEY_GPIO) == 0;
#else
    return false;
#endif
}

#if CONFIG_OBSERVORE_POWER_KEY_GPIO >= 0
static void power_task(void *arg)
{
    (void)arg;
    int64_t down_since = 0;
    for (;;) {
        if (observore_power_button()) {
            int64_t now = esp_timer_get_time();
            if (!down_since) {
                down_since = now;
            } else if ((now - down_since) / 1000 >= OFF_HOLD_MS) {
                /* Said before it happens, because the next instruction may
                 * be the last one this board executes. On USB the latch is
                 * irrelevant and the board stays up, which is the right
                 * behaviour: a press cannot cut power that is not coming
                 * from the cell. */
                ESP_LOGW(TAG, "power button held %d ms -- releasing the "
                              "latch", OFF_HOLD_MS);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(CONFIG_OBSERVORE_POWER_HOLD_GPIO, 0);
                s_held = false;
                /* Still here means USB is supplying the board. Re-latch, so
                 * that pulling the cable later does not drop it. */
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(CONFIG_OBSERVORE_POWER_HOLD_GPIO, 1);
                s_held = true;
                ESP_LOGW(TAG, "still running, so this is USB power; latch "
                              "restored");
                down_since = 0;
            }
        } else {
            down_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void observore_power_start(void)
{
    if (!s_held) {
        return;
    }
#if CONFIG_OBSERVORE_POWER_KEY_GPIO >= 0
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_OBSERVORE_POWER_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&in) != ESP_OK) {
        ESP_LOGW(TAG, "no power button on GPIO%d; the board can only be "
                      "switched off by flattening or unplugging the cell",
                 CONFIG_OBSERVORE_POWER_KEY_GPIO);
        return;
    }
    xTaskCreate(power_task, "observore_pwr", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "power button on GPIO%d; hold %d ms to switch off",
             CONFIG_OBSERVORE_POWER_KEY_GPIO, OFF_HOLD_MS);
#endif
}

#else  /* this board holds its own power up */

void observore_power_hold(void) {}
void observore_power_start(void) {}
bool observore_power_button(void) { return false; }

#endif
