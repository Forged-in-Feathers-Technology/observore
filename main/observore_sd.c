#include "observore_sd.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "observore.sd";

#if CONFIG_OBSERVORE_SDCARD

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "observore_ioexp.h"

#define MOUNT_POINT "/sd"

static sdmmc_card_t *s_card;
static bool s_ready;
static char s_name[32];

void observore_sd_init(void)
{
    /* Deselected first, and that ordering is the whole of the trick.
     *
     * A card enters SPI mode only after it has seen at least 74 clock
     * cycles with its select line HIGH. The first version of this pulled
     * select low before the bus even existed, reasoning that nothing else
     * is on these wires so it could simply stay low -- and the card then
     * detected on roughly one boot in three, which is exactly what a card
     * that sometimes misses its entry sequence looks like.
     *
     * So: high, clock the bus idle, then low for the rest of time. */
    if (!observore_ioexp_set(CONFIG_OBSERVORE_SDCARD_CS_IOEXP_BIT, true)) {
        ESP_LOGW(TAG, "could not reach the card's select line on expander "
                      "bit %d; not mounting",
                 CONFIG_OBSERVORE_SDCARD_CS_IOEXP_BIT);
        return;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_OBSERVORE_SDCARD_MOSI,
        .miso_io_num = CONFIG_OBSERVORE_SDCARD_MISO,
        .sclk_io_num = CONFIG_OBSERVORE_SDCARD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(CONFIG_OBSERVORE_SDCARD_SPI_HOST, &bus,
                           SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "could not take SPI%d for the card",
                 CONFIG_OBSERVORE_SDCARD_SPI_HOST);
        return;
    }

    /* The entry sequence: 80 clocks with select high and data high.
     *
     * Sent through a temporary device with no select of its own, because
     * the real one is on an I2C expander and the SPI peripheral cannot
     * drive it. Ten bytes of 0xFF is 80 clocks, comfortably over the 74 the
     * card is entitled to. */
    spi_device_interface_config_t idle = {
        .clock_speed_hz = 400 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    spi_device_handle_t clocker;
    if (spi_bus_add_device(CONFIG_OBSERVORE_SDCARD_SPI_HOST, &idle,
                           &clocker) == ESP_OK) {
        uint8_t ones[10];
        memset(ones, 0xFF, sizeof(ones));
        spi_transaction_t t = {
            .length = sizeof(ones) * 8,
            .tx_buffer = ones,
        };
        spi_device_transmit(clocker, &t);
        spi_bus_remove_device(clocker);
    } else {
        ESP_LOGW(TAG, "could not clock the card into SPI mode; it may not "
                      "be found");
    }

    /* Now selected, and it stays that way. Nothing else is on these three
     * wires, so the alternative -- an I2C round trip around every SPI
     * transaction -- would cost more than it bought. */
    observore_ioexp_set(CONFIG_OBSERVORE_SDCARD_CS_IOEXP_BIT, false);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CONFIG_OBSERVORE_SDCARD_SPI_HOST;
    /* Conservative. A card that will not talk at all is a different problem
     * from one that will not talk quickly, and this is bring-up. */
    host.max_freq_khz = CONFIG_OBSERVORE_SDCARD_KHZ;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = CONFIG_OBSERVORE_SDCARD_SPI_HOST;
    /* The driver is told there is no select to manage, because the one this
     * board has is not a pin it can reach. It is already low. */
    dev.gpio_cs = SDSPI_SLOT_NO_CS;
    dev.gpio_cd = SDSPI_SLOT_NO_CD;
    dev.gpio_wp = SDSPI_SLOT_NO_WP;
    dev.gpio_int = GPIO_NUM_NC;

    /* Never format. A card arriving with something already on it belongs to
     * whoever put it there, and a detector that wipes the evidence it was
     * handed is worse than one with no slot. An unreadable card is reported
     * and left alone. */
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev,
                                            &mount, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "a card answered but its filesystem would not "
                          "mount -- FAT32 is what this expects, and it is "
                          "not formatted here on purpose");
        } else {
            ESP_LOGI(TAG, "no card (%s)", esp_err_to_name(err));
        }
        spi_bus_free(CONFIG_OBSERVORE_SDCARD_SPI_HOST);
        observore_ioexp_set(CONFIG_OBSERVORE_SDCARD_CS_IOEXP_BIT, true);
        return;
    }

    s_ready = true;
    snprintf(s_name, sizeof(s_name), "%s", s_card->cid.name);

#if CONFIG_OBSERVORE_SDCARD_BENCH
    /* Bring-up only: does it write, and how fast?
     *
     * Mounting proves the card can be read. Whether it can be written, and
     * at what rate, is what decides whether this is somewhere to put a
     * detection history or just somewhere to read a config from -- and
     * guessing either would be guessing. */
    {
        enum { CHUNK = 4096, CHUNKS = 32 };   /* 128 KB */
        static uint8_t buf[CHUNK];
        memset(buf, 0xA5, sizeof(buf));
        int64_t t0 = esp_timer_get_time();
        FILE *f = fopen(MOUNT_POINT "/bench.tmp", "wb");
        if (!f) {
            /* errno, not a guess. "read-only, or full" was the first
             * version of this line and it named two causes without
             * checking either; EROFS, ENOSPC, ENOENT and EACCES are four
             * different problems and the C library already knows which. */
            ESP_LOGW(TAG, "bench: mounted, but opening a file for writing "
                          "failed: errno %d (%s)", errno, strerror(errno));
        } else {
            size_t wrote = 0;
            for (int i = 0; i < CHUNKS; i++) {
                wrote += fwrite(buf, 1, sizeof(buf), f);
            }
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            int64_t us = esp_timer_get_time() - t0;
            ESP_LOGW(TAG, "bench: wrote %u bytes in %lld ms = %lld KB/s",
                     (unsigned)wrote, us / 1000,
                     us > 0 ? (int64_t)wrote * 1000 / us : 0);
            t0 = esp_timer_get_time();
            f = fopen(MOUNT_POINT "/bench.tmp", "rb");
            size_t got = 0;
            if (f) {
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) got += n;
                fclose(f);
            }
            us = esp_timer_get_time() - t0;
            ESP_LOGW(TAG, "bench: read  %u bytes in %lld ms = %lld KB/s",
                     (unsigned)got, us / 1000,
                     us > 0 ? (int64_t)got * 1000 / us : 0);
            unlink(MOUNT_POINT "/bench.tmp");
        }
    }
#endif

    uint32_t total = 0, freemb = 0;
    observore_sd_space(&total, &freemb);
    ESP_LOGI(TAG, "card \"%s\" mounted at %s: %lu MB, %lu MB free, %d kHz",
             s_name, MOUNT_POINT, (unsigned long)total, (unsigned long)freemb,
             CONFIG_OBSERVORE_SDCARD_KHZ);
}

bool observore_sd_ready(void) { return s_ready; }

void observore_sd_space(uint32_t *total_mb, uint32_t *free_mb)
{
    if (total_mb) *total_mb = 0;
    if (free_mb)  *free_mb  = 0;
    if (!s_ready) {
        return;
    }
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &avail) != ESP_OK) {
        return;
    }
    if (total_mb) *total_mb = (uint32_t)(total / (1024 * 1024));
    if (free_mb)  *free_mb  = (uint32_t)(avail / (1024 * 1024));
}

const char *observore_sd_name(void) { return s_ready ? s_name : ""; }

#else  /* no card slot on this board */

void observore_sd_init(void) {}
bool observore_sd_ready(void) { return false; }
void observore_sd_space(uint32_t *t, uint32_t *f)
{
    if (t) *t = 0;
    if (f) *f = 0;
}
const char *observore_sd_name(void) { return ""; }

#endif
