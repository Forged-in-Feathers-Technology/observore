#include "observore_touch.h"

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_TOUCH

#include <stdint.h>
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

/* Bool options that are off are undefined, not zero, so give the one read as a
 * value a definite one. */
#ifndef CONFIG_OBSERVORE_TOUCH_SHARED_BUS
#define CONFIG_OBSERVORE_TOUCH_SHARED_BUS 0
#endif

/* Its own bus where the board gives it one, the panel's where it does not. */
#if CONFIG_OBSERVORE_TOUCH_SHARED_BUS
#define TOUCH_HOST SPI2_HOST
#else
#define TOUCH_HOST SPI3_HOST
#endif

/* Control bytes: start, channel, 12-bit, differential, power-down between
 * conversions. Y and X are the two position channels; Z1 is what tells us a
 * finger is actually on the glass rather than the line merely floating. */
#define CMD_Y  0x90
#define CMD_X  0xD0
#define CMD_Z1 0xB0
#define CMD_Z2 0xC0

/* How hard the glass is being pressed, from both Z channels rather than one.
 *
 * Z1 alone scales with where on the sheet the touch is, so a single threshold
 * against it is really a threshold against position: on the 3.5" board the
 * button bar sits at the end where Z1 reads small, and firm presses there
 * measured about 130 against a limit of 300 -- every one of them discarded
 * before it could become a tap, on a panel that was working perfectly.
 *
 * Z1 rises and Z2 falls under a finger, so their combination is a far flatter
 * measure across the sheet. Untouched it sits near zero; a deliberate press is
 * in the hundreds. */
#define Z_THRESHOLD CONFIG_OBSERVORE_TOUCH_Z_MIN


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
/* Five SPI transactions and a five-element sort need very little -- but this
 * task also logs, and formatting one line costs about a kilobyte of stack on
 * this chip. Cut to 1536 by eye during a heap fix, it survived the shipping
 * build and overflowed the moment CONFIG_OBSERVORE_TOUCH_LOG_RAW was turned
 * on, which is the procedure the README tells people to follow to calibrate a
 * panel: every touch panicked the device. Sized from the high-water mark the
 * task now reports rather than guessed again: 2560 left only 476 bytes spare
 * once a touch had been logged, which is a margin in name only. */
#define TASK_STACK 3072

static spi_device_handle_t s_dev;
static bool s_ready;
static int64_t s_task_started_us;

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

/* How hard the glass is being pressed. See Z_THRESHOLD above for why both
 * channels are read rather than just the first. */
static int pressure(void)
{
    int z1 = median_channel(CMD_Z1);
    int z2 = median_channel(CMD_Z2);
    if (z1 < 0 || z2 < 0) {
        return -1;
    }
    return z1 + 4095 - z2;
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

    /* Some boards give the controller its own pins and some hang it off the
     * panel's bus with a second chip select. Sharing is not merely allowed --
     * the driver serialises devices on one bus, so a measurement can never
     * land in the middle of a screen update. */
    const spi_host_device_t host = TOUCH_HOST;
    esp_err_t err;
#if !CONFIG_OBSERVORE_TOUCH_SHARED_BUS
    spi_bus_config_t bus = {
        .mosi_io_num     = CONFIG_OBSERVORE_TOUCH_MOSI,
        .miso_io_num     = CONFIG_OBSERVORE_TOUCH_MISO,
        .sclk_io_num     = CONFIG_OBSERVORE_TOUCH_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32,
    };
    err = spi_bus_initialize(host, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return;
    }
#endif

    spi_device_interface_config_t dev = {
        .clock_speed_hz = TOUCH_HZ,
        .mode           = 0,
        .spics_io_num   = CONFIG_OBSERVORE_TOUCH_CS,
        .queue_size     = 1,
    };
    err = spi_bus_add_device(host, &dev, &s_dev);
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
    ESP_LOGI(TAG, "XPT2046 ready on SPI%d (irq %d)%s", TOUCH_HOST + 1,
             CONFIG_OBSERVORE_TOUCH_IRQ,
             CONFIG_OBSERVORE_TOUCH_SHARED_BUS ? ", sharing the panel's bus" : "");
    s_task_started_us = esp_timer_get_time();
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
#if CONFIG_OBSERVORE_TOUCH_LOG_RAW
    /* Bring-up: say what the interrupt line is doing and what the controller
     * answers even when the gate says nobody is touching. A panel that never
     * reports could be a wrong interrupt pin, a controller that is not
     * answering at all, or a pressure threshold set too high, and those look
     * identical from outside. */
    static int64_t s_last_probe_us;
    int64_t now_probe = esp_timer_get_time();
    if (now_probe - s_last_probe_us > 500 * 1000) {
        s_last_probe_us = now_probe;
        int lvl = gpio_get_level(CONFIG_OBSERVORE_TOUCH_IRQ);
        int z   = s_ready ? pressure() : -1;
        int rx  = s_ready ? median_channel(CMD_X)  : -1;
        int ry  = s_ready ? median_channel(CMD_Y)  : -1;
        ESP_LOGI(TAG, "probe irq=%d z=%4d x=%4d y=%4d", lvl, z, rx, ry);
    }
#endif
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
    int z = pressure();
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
    /* Said once, half a minute in, by which time a panel has usually been
     * touched and the logging path exercised. Stack headroom is as much a
     * budget as free heap on this board, and the only way to size it is to
     * read it back. */
    /* Follows the mark down rather than saying it once, the way the heap
     * watch does: the expensive path here is formatting a log line, which
     * only happens when someone is actually touching the panel. Saying it
     * once, before that, is how a stack gets sized wrongly. */
    size_t reported = SIZE_MAX;
    for (;;) {
        size_t spare = uxTaskGetStackHighWaterMark(NULL);
        if (spare + 64 < reported) {
            reported = spare;
            ESP_LOGI(TAG, "%s: %u bytes of %d", "stack headroom",
                     (unsigned)spare, TASK_STACK);
        }
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
