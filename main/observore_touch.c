#include "observore_touch.h"

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_TOUCH

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "observore_display.h"

static const char *TAG = "observore.touch";

/* The XPT2046 wants its own, much slower clock than the panel: 2 MHz is inside
 * every datasheet figure and the pins are routed through the GPIO matrix, not
 * the IOMUX, so there is nothing to gain from pushing it. */
#define TOUCH_HZ (2 * 1000 * 1000)

/* Control bytes: start, channel, 12-bit, differential, power-down between
 * conversions. Y and X are the two position channels; Z1 is what tells us a
 * finger is actually on the glass rather than the line merely floating. */
#define CMD_Y  0x90
#define CMD_X  0xD0
#define CMD_Z1 0xB0

/* Below this the press is too light to trust a position from. Measured on the
 * bench: a firm touch reads in the thousands, a lifted finger under a hundred,
 * and the band between is where a wrong coordinate comes from. */
#define Z_THRESHOLD 300

/* Samples per axis per read. Resistive panels are noisy and a median throws
 * away the outlier that a mean would average into the answer. */
#define SAMPLES 5

/* How often the panel is asked. Fast enough that a button feels like a button,
 * and cheap because a poll with no finger on the glass is one GPIO read.
 *
 * It has to be its own task rather than a call from the main loop: a patrol
 * cycle spends about thirty seconds inside a blocking passive scan, and a
 * screen that ignores a finger for thirty seconds is a screen nobody trusts. */
#define POLL_MS   20
#define TASK_STACK 2560

static spi_device_handle_t s_dev;
static bool s_ready;

/* Press state, owned by the poll task and read under the lock. */
static SemaphoreHandle_t s_lock;
static bool    s_down;
static int     s_x, s_y;          /* where the finger is now */
static int     s_down_x, s_down_y; /* where it went down */
static int64_t s_down_us;
static bool    s_tap_pending;
static int     s_tap_x, s_tap_y;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void touch_task(void *arg);

static int read_channel(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};
    spi_transaction_t t = {
        .length    = sizeof(tx) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) {
        return -1;
    }
    /* The conversion straddles the second and third bytes, left-aligned: the
     * top bit of the 16-bit window is the controller's busy bit. */
    return (int)(((uint16_t)rx[1] << 8 | rx[2]) >> 3) & 0x0FFF;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int median_channel(uint8_t cmd)
{
    int v[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        v[i] = read_channel(cmd);
        if (v[i] < 0) {
            return -1;
        }
    }
    qsort(v, SAMPLES, sizeof(v[0]), cmp_int);
    return v[SAMPLES / 2];
}

void observore_touch_init(void)
{
    /* PENIRQ: low while the panel is touched. Polling this costs one register
     * read, so the SPI bus stays idle until there is something to read. */
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << CONFIG_OBSERVORE_TOUCH_IRQ,
        .mode         = GPIO_MODE_INPUT,
    };
    gpio_config(&irq);

    spi_bus_config_t bus = {
        .mosi_io_num     = CONFIG_OBSERVORE_TOUCH_MOSI,
        .miso_io_num     = CONFIG_OBSERVORE_TOUCH_MISO,
        .sclk_io_num     = CONFIG_OBSERVORE_TOUCH_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = TOUCH_HZ,
        .mode           = 0,
        .spics_io_num   = CONFIG_OBSERVORE_TOUCH_CS,
        .queue_size     = 1,
    };
    err = spi_bus_add_device(SPI3_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device: %s", esp_err_to_name(err));
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no memory for the touch lock");
        return;
    }
    s_ready = true;
    if (xTaskCreate(touch_task, "touch", TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the touch task");
        s_ready = false;
        return;
    }
    ESP_LOGI(TAG, "XPT2046 ready on SPI3 (irq %d)", CONFIG_OBSERVORE_TOUCH_IRQ);
}

/* Raw ADC counts to screen pixels.
 *
 * Three transforms, all build options, because all three are properties of one
 * assembly rather than of the product: the bounds of the resistive sheet,
 * whether its axes line up with the display's after rotation, and which way
 * each one runs. On the 2.8" board measured here the touch axes happen NOT to
 * be crossed relative to the landscape panel -- raw X is the screen's
 * horizontal -- and raw Y counts from the bottom up. That is the opposite of
 * what the display's own swap_xy suggests, because the sheet is bonded in its
 * own orientation and wired to suit the glass, not the controller.
 *
 * Each is visible when wrong and in a different way: bad bounds put the finger
 * a fixed distance from the mark, a missing swap sends it along the wrong
 * edge, and a missing flip sends it the opposite way. */
static void to_screen(int raw_x, int raw_y, int *x, int *y)
{
    int lo_x = CONFIG_OBSERVORE_TOUCH_RAW_MIN_X, hi_x = CONFIG_OBSERVORE_TOUCH_RAW_MAX_X;
    int lo_y = CONFIG_OBSERVORE_TOUCH_RAW_MIN_Y, hi_y = CONFIG_OBSERVORE_TOUCH_RAW_MAX_Y;

    /* Per-mille of the way along each raw axis, so the whole mapping is
     * integer arithmetic on a chip with no reason to spend a float here. */
    int fx = (raw_x - lo_x) * 1000 / (hi_x - lo_x ? hi_x - lo_x : 1);
    int fy = (raw_y - lo_y) * 1000 / (hi_y - lo_y ? hi_y - lo_y : 1);

#if CONFIG_OBSERVORE_TOUCH_SWAP_XY
    int t = fx; fx = fy; fy = t;
#endif
#if CONFIG_OBSERVORE_TOUCH_FLIP_X
    fx = 1000 - fx;
#endif
#if CONFIG_OBSERVORE_TOUCH_FLIP_Y
    fy = 1000 - fy;
#endif

    int sx = fx * OBSERVORE_DISPLAY_W / 1000;
    int sy = fy * OBSERVORE_DISPLAY_H / 1000;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= OBSERVORE_DISPLAY_W) sx = OBSERVORE_DISPLAY_W - 1;
    if (sy >= OBSERVORE_DISPLAY_H) sy = OBSERVORE_DISPLAY_H - 1;
    *x = sx;
    *y = sy;
}

/* One sampled position, straight off the controller. Only the poll task calls
 * this: two tasks sharing one SPI device would interleave a command with the
 * other's reply. */
static bool sample(int *x, int *y)
{
    if (!s_ready || gpio_get_level(CONFIG_OBSERVORE_TOUCH_IRQ) != 0) {
        return false;
    }
    /* The backlight is a PWM whose switching sits inside the controller's
     * measuring band, so it is held steady across a measurement. Whether it
     * actually matters could not be settled on the bench: the panel there lost
     * one of its two plates partway through bring-up, which masks any smaller
     * effect. It costs about a millisecond, inside the interrupt gate, so it
     * only ever happens with a finger already on the glass. */
    /* The backlight is a PWM whose switching sits inside the controller's
     * measuring band, so it is held steady across a measurement. It costs
     * about a millisecond, inside the interrupt gate, so it only ever happens
     * with a finger already on the glass. */
    observore_display_backlight_hold();
    int z = median_channel(CMD_Z1);
    int raw_x = z >= Z_THRESHOLD ? median_channel(CMD_X) : -1;
    int raw_y = z >= Z_THRESHOLD ? median_channel(CMD_Y) : -1;
    observore_display_backlight_release();

    if (z < Z_THRESHOLD || raw_x < 0 || raw_y < 0) {
        return false;
    }
#if CONFIG_OBSERVORE_TOUCH_LOG_RAW
    /* Bring-up aid: what the glass actually reports, so the bounds above can
     * be set from the panel in hand rather than from someone else's. */
    ESP_LOGI(TAG, "raw x=%4d y=%4d z=%4d", raw_x, raw_y, z);
#endif
    to_screen(raw_x, raw_y, x, y);
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;
    for (;;) {
        int sx = 0, sy = 0;
        bool down = sample(&sx, &sy);
        int64_t now = esp_timer_get_time();

        LOCK();
        if (down && !s_down) {
            s_down    = true;
            s_down_x  = sx;
            s_down_y  = sy;
            s_down_us = now;
        } else if (!down && s_down) {
            /* A contact too brief to be deliberate is electrical noise from
             * the radio alongside it, not a finger. A tap reports where the
             * finger went down, not where it left: a thumb rolls a few pixels
             * on release and a button should not care. */
            if (now - s_down_us >= 30 * 1000) {
                s_tap_pending = true;
                s_tap_x = s_down_x;
                s_tap_y = s_down_y;
            }
            s_down = false;
        }
        if (down) {
            s_x = sx;
            s_y = sy;
        }
        UNLOCK();

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool observore_touch_read(int *x, int *y)
{
    if (!s_ready) {
        return false;
    }
    LOCK();
    bool down = s_down;
    if (down) {
        if (x) *x = s_x;
        if (y) *y = s_y;
    }
    UNLOCK();
    return down;
}

bool observore_touch_tap(int *x, int *y)
{
    if (!s_ready) {
        return false;
    }
    LOCK();
    bool got = s_tap_pending;
    if (got) {
        s_tap_pending = false;
        if (x) *x = s_tap_x;
        if (y) *y = s_tap_y;
    }
    UNLOCK();
    return got;
}

#else  /* no touch panel configured */

void observore_touch_init(void) {}
bool observore_touch_read(int *x, int *y) { (void)x; (void)y; return false; }
bool observore_touch_tap(int *x, int *y)  { (void)x; (void)y; return false; }

#endif
