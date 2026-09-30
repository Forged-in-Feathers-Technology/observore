#include "observore_i2c.h"

#if CONFIG_OBSERVORE_I2C

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "observore.i2c";

#define I2C_TIMEOUT_MS 200

static i2c_master_bus_handle_t s_bus;
static bool s_tried;

i2c_master_bus_handle_t observore_i2c_bus(void)
{
    if (s_bus || s_tried) {
        return s_bus;
    }
    s_tried = true;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_OBSERVORE_I2C_SDA,
        .scl_io_num = CONFIG_OBSERVORE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus on sda %d / scl %d: %s", CONFIG_OBSERVORE_I2C_SDA,
                 CONFIG_OBSERVORE_I2C_SCL, esp_err_to_name(err));
        s_bus = NULL;
    }
    return s_bus;
}

i2c_master_dev_handle_t observore_i2c_device(uint8_t addr, uint32_t hz)
{
    i2c_master_bus_handle_t bus = observore_i2c_bus();
    if (!bus) {
        return NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "could not add 0x%02X", addr);
        return NULL;
    }
    return dev;
}

bool observore_i2c_read(i2c_master_dev_handle_t dev, uint8_t reg,
                        uint8_t *buf, size_t len)
{
    if (!dev) {
        return false;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len,
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK;
}

bool observore_i2c_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    if (!dev) {
        return false;
    }
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf),
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK;
}

bool observore_i2c_write_bytes(i2c_master_dev_handle_t dev,
                               const uint8_t *buf, size_t len)
{
    if (!dev) {
        return false;
    }
    return i2c_master_transmit(dev, buf, len,
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK;
}

void observore_i2c_scan(char *out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    for (uint8_t addr = 0x08; addr < 0x78 && n + 6 < len; addr++) {
        i2c_master_dev_handle_t dev = observore_i2c_device(addr, 300 * 1000);
        if (!dev) {
            continue;
        }
        uint8_t val = 0;
        if (observore_i2c_read(dev, 0x00, &val, 1)) {
            n += (size_t)snprintf(out + n, len - n, " 0x%02X", addr);
        }
        i2c_master_bus_rm_device(dev);
    }
}

#endif
