#include "observore_i2c.h"

#if CONFIG_OBSERVORE_I2C

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "observore.i2c";

#define I2C_TIMEOUT_MS 200

/* One entry per bus, brought up on first use. A board with one bus never
 * touches the second slot, and asking for a bus a profile gave no pins to
 * returns NULL rather than inventing one. */
static struct {
    i2c_master_bus_handle_t handle;
    bool                    tried;
} s_buses[2];

static void bus_pins(observore_i2c_bus_id_t id, int *sda, int *scl)
{
    if (id == OBSERVORE_I2C_AUX) {
        *sda = CONFIG_OBSERVORE_I2C_AUX_SDA;
        *scl = CONFIG_OBSERVORE_I2C_AUX_SCL;
    } else {
        *sda = CONFIG_OBSERVORE_I2C_SDA;
        *scl = CONFIG_OBSERVORE_I2C_SCL;
    }
}

i2c_master_bus_handle_t observore_i2c_bus_of(observore_i2c_bus_id_t id)
{
    if (id > OBSERVORE_I2C_AUX) {
        return NULL;
    }
    if (s_buses[id].handle || s_buses[id].tried) {
        return s_buses[id].handle;
    }
    s_buses[id].tried = true;

    int sda, scl;
    bus_pins(id, &sda, &scl);
    if (sda < 0 || scl < 0) {
        /* Not a fault: a board with one bus leaves the second unconfigured,
         * and the caller that wanted it has somewhere sensible to go. */
        return NULL;
    }
    i2c_master_bus_config_t cfg = {
        /* -1 lets the driver pick a free port, so two buses cannot be given
         * the same one by a profile that forgot. */
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_buses[id].handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s bus on sda %d / scl %d: %s",
                 id == OBSERVORE_I2C_AUX ? "aux" : "main", sda, scl,
                 esp_err_to_name(err));
        s_buses[id].handle = NULL;
    }
    return s_buses[id].handle;
}

i2c_master_bus_handle_t observore_i2c_bus(void)
{
    return observore_i2c_bus_of(OBSERVORE_I2C_MAIN);
}

i2c_master_dev_handle_t observore_i2c_device_on(observore_i2c_bus_id_t id,
                                                uint8_t addr, uint32_t hz)
{
    i2c_master_bus_handle_t bus = observore_i2c_bus_of(id);
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

i2c_master_dev_handle_t observore_i2c_device(uint8_t addr, uint32_t hz)
{
    return observore_i2c_device_on(OBSERVORE_I2C_MAIN, addr, hz);
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

void observore_i2c_scan_on(observore_i2c_bus_id_t id, char *out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    for (uint8_t addr = 0x08; addr < 0x78 && n + 6 < len; addr++) {
        i2c_master_dev_handle_t dev =
            observore_i2c_device_on(id, addr, 300 * 1000);
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

void observore_i2c_scan(char *out, size_t len)
{
    observore_i2c_scan_on(OBSERVORE_I2C_MAIN, out, len);
}

#endif
