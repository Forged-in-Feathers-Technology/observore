#include "observore_ioexp.h"

#if CONFIG_OBSERVORE_IOEXP

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "observore_i2c.h"

static const char *TAG = "observore.ioexp";

/* TCA9554 registers. Config is the one that catches people out: a bit SET
 * there makes the line an input, so driving anything means clearing it. */
#define REG_INPUT   0x00
#define REG_OUTPUT  0x01
#define REG_POLARITY 0x02
#define REG_CONFIG  0x03

static i2c_master_dev_handle_t s_dev;
static uint8_t                 s_shadow;    /* what we last drove */
static uint8_t                 s_outputs;   /* which lines we have claimed */

bool observore_ioexp_present(void)
{
    return s_dev != NULL;
}

void observore_ioexp_init(void)
{
    if (s_dev) {
        return;
    }
    /* On the main bus deliberately. On the 1.85 the expander shares pins with
     * the clock and the accelerometer, not with the touch panel -- which a
     * scan of the board reported and the wiki did not say. */
    s_dev = observore_i2c_device(CONFIG_OBSERVORE_IOEXP_I2C_ADDR, 400 * 1000);
    if (!s_dev) {
        ESP_LOGW(TAG, "no expander at 0x%02X", CONFIG_OBSERVORE_IOEXP_I2C_ADDR);
        return;
    }

    /* Every line left an INPUT, and only driven when something asks for it.
     *
     * The first version made all eight outputs at once, which is wrong on
     * this board: two of them are the accelerometer's interrupt lines, and
     * the accelerometer drives those. Configuring them as outputs puts two
     * chips on one wire -- it did no visible harm on the bench, which is
     * exactly how that kind of mistake survives.
     *
     * So: inputs until asked. A reset line spends its idle life undriven and
     * held by its pull-up, which is what it would have on a GPIO anyway. */
    s_shadow = 0x00;
    s_outputs = 0x00;
    bool ok = observore_i2c_write(s_dev, REG_POLARITY, 0x00) &&
              observore_i2c_write(s_dev, REG_OUTPUT, s_shadow) &&
              observore_i2c_write(s_dev, REG_CONFIG, 0xFF);
    if (!ok) {
        ESP_LOGW(TAG, "expander at 0x%02X did not take its configuration",
                 CONFIG_OBSERVORE_IOEXP_I2C_ADDR);
        s_dev = NULL;
        return;
    }

    /* Read back rather than trust the write. The chip answers a probe whether
     * or not it accepted anything, and a reset line that silently stayed an
     * input is a panel that never comes up for a reason nothing reports. */
    uint8_t cfg = 0x00;
    if (observore_i2c_read(s_dev, REG_CONFIG, &cfg, 1) && cfg != 0xFF) {
        ESP_LOGW(TAG, "expander kept config 0x%02X rather than 0xFF; it did "
                      "not take its configuration", cfg);
        s_dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "expander at 0x%02X, eight lines idle as inputs",
             CONFIG_OBSERVORE_IOEXP_I2C_ADDR);
}

bool observore_ioexp_set(uint8_t bit, bool high)
{
    if (!s_dev || bit > 7) {
        return false;
    }
    uint8_t next = high ? (uint8_t)(s_shadow | (1u << bit))
                        : (uint8_t)(s_shadow & ~(1u << bit));
    /* The level first, then the direction, so a line claimed for the first
     * time does not drive the opposite of what was asked for the instant it
     * becomes an output. */
    if (!observore_i2c_write(s_dev, REG_OUTPUT, next)) {
        return false;
    }
    if (!(s_outputs & (1u << bit))) {
        uint8_t cfg = (uint8_t)~(s_outputs | (1u << bit));
        if (!observore_i2c_write(s_dev, REG_CONFIG, cfg)) {
            return false;
        }
        s_outputs = (uint8_t)(s_outputs | (1u << bit));
    }
    /* Kept only after the write succeeded, so a failed write does not leave
     * this believing it drove something it did not. */
    s_shadow = next;
    return true;
}

bool observore_ioexp_reset_pulse(uint8_t bit, int low_ms, int settle_ms)
{
    if (!observore_ioexp_set(bit, false)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(low_ms > 0 ? low_ms : 1));
    if (!observore_ioexp_set(bit, true)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(settle_ms > 0 ? settle_ms : 1));
    return true;
}

#else  /* !CONFIG_OBSERVORE_IOEXP */

bool observore_ioexp_present(void) { return false; }
void observore_ioexp_init(void) { }
bool observore_ioexp_set(uint8_t bit, bool high) { (void)bit; (void)high; return false; }
bool observore_ioexp_reset_pulse(uint8_t b, int l, int s)
{
    (void)b; (void)l; (void)s;
    return false;
}

#endif /* CONFIG_OBSERVORE_IOEXP */
