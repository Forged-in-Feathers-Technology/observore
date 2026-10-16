#include "observore_tap.h"
#include "observore_touchcal.h"
#include "observore_touch.h"

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_TOUCH

#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#if CONFIG_OBSERVORE_TOUCH_CAPACITIVE
#include "observore_i2c.h"
#include "observore_ioexp.h"
#else
#include "driver/spi_master.h"
#endif
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "observore_display.h"
#include "observore_nvs.h"

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

/* The largest reading the controller can return: the XPT2046's converter is
 * twelve bits. Anything outside 0..RAW_MAX was computed, not measured. */
#define RAW_MAX 4095


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

/* How long a loss of contact must persist before a press is over.
 *
 * Zero where the controller's interrupt line is wired: the pressure reading
 * and the interrupt agree before a press ends, so a single sub-threshold
 * sample is already corroborated, and these boards work as they are.
 *
 * On a board with no interrupt line -- the NM-CYD-C5, whose vendor pin table
 * prints "---" for it -- pressure is the only witness, and one 20 ms sample
 * is enough to end a press a finger is still making. Three polls is long
 * enough to ride out a dip and far shorter than the gap between two
 * keystrokes, which is what stops it merging them. */
#if CONFIG_OBSERVORE_TOUCH_POLLED
#define RELEASE_US (3 * POLL_MS * 1000)
#else
#define RELEASE_US 0
#endif
/* Five SPI transactions and a five-element sort need very little -- but this
 * task also logs, and formatting one line costs about a kilobyte of stack on
 * this chip. Cut to 1536 by eye during a heap fix, it survived the shipping
 * build and overflowed the moment CONFIG_OBSERVORE_TOUCH_LOG_RAW was turned
 * on, which is the procedure docs/display.md tells people to follow to
 * calibrate a panel: every touch panicked the device. Sized from the
 * high-water mark the task now reports rather than guessed again: 2560 left
 * only 476 bytes spare once a touch had been logged, which is a margin in
 * name only. */
#define TASK_STACK 3072

#if CONFIG_OBSERVORE_TOUCH_XPT2046
static spi_device_handle_t s_dev;
#endif
static bool s_ready;
static int64_t s_task_started_us;

/* Press state, owned by the poll task and read under the lock. */
static SemaphoreHandle_t s_lock;
static bool    s_down;            /* mirrors s_tap.down, for the public read */
static int     s_x, s_y;          /* where the finger is now */
static observore_tap_state_t s_tap;
static bool    s_tap_pending;
static int     s_tap_x, s_tap_y;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void touch_task(void *arg);

/* ---- Shared by both backends: the raw-to-screen mapping and its
 * calibration.
 *
 * This used to sit inside the resistive half, on the strength of a comment
 * saying a capacitive controller reports "in panel pixels, so there is no
 * calibration to get wrong". That is a claim about one chip's factory
 * configuration, not a property of the interface: the CST816 family is
 * fitted to round panels of several sizes and reports in whatever range it
 * was configured for. Where that is not the panel's own, every reading is
 * scaled -- right at the top-left corner and progressively wrong towards
 * the bottom-right, which is the one shape of error the assumption made
 * impossible to express and impossible to correct.
 *
 * So both backends map through the same arithmetic now, and both can be
 * calibrated on the glass. For a controller that really does report panel
 * pixels the bounds are the panel's size and the mapping is the identity,
 * so nothing changes for the 1.43" where touch already lands where pressed.
 */
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
/* The bounds in use. Compiled-in values to begin with -- one bench panel's
 * measurements -- replaced by whatever this device has been taught about its
 * own sheet. */
static int  s_lo_x = CONFIG_OBSERVORE_TOUCH_RAW_MIN_X;
static int  s_hi_x = CONFIG_OBSERVORE_TOUCH_RAW_MAX_X;
static int  s_lo_y = CONFIG_OBSERVORE_TOUCH_RAW_MIN_Y;
static int  s_hi_y = CONFIG_OBSERVORE_TOUCH_RAW_MAX_Y;
static bool s_calibrated;

/* Four numbers, as they are stored, and which arithmetic produced them.
 *
 * The version is here because of #149. Until that fix the calibration scaled
 * each touch channel by the *other* axis's ratio, so every calibration saved
 * before it is wrong -- and an upgrade that kept one would look exactly like
 * the bug never being fixed, because the symptom is identical.
 *
 * The first attempt at healing that tried to recognise bad bounds by their
 * values: a span wider than the converter can read, an endpoint below zero.
 * That does not work, and the tests are what said so. A correct calibration
 * extrapolates from inset targets assuming a linear sheet, and near the bezel
 * the sheet is not quite linear, so it can legitimately overshoot both ends by
 * a few counts -- which makes its span exceed the range too. One bench board's
 * bad bounds were 136 counts over, well inside any tolerance wide enough to
 * admit a real one. And on a panel whose usable range is narrower, the same
 * bug produces bounds entirely inside 0..4095 and is undetectable by value at
 * all.
 *
 * So this keys on provenance instead of trying to infer wrongness. A stored
 * calibration says which arithmetic made it, and one that does not say, or
 * says the old one, is discarded. Unversioned blobs are eight bytes and this
 * is ten, so they fail the length check without needing a value in them. */
#define TOUCH_CAL_VERSION 2

typedef struct {
    uint16_t version;
    int16_t  lo_x, hi_x, lo_y, hi_y;
} touch_cal_t;

static void cal_load(void)
{
    touch_cal_t cal = {0};
    observore_nvs_item_t item = {.key = "touchcal", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &cal, .len = sizeof(cal)};
    if (observore_nvs_read(&item, 1) != ESP_OK || !item.found) {
        return;
    }
    if (item.len != sizeof(cal) || cal.version != TOUCH_CAL_VERSION) {
        ESP_LOGW(TAG, "stored calibration was written by an older build "
                      "(%u bytes, version %u) -- using the built-in bounds. "
                      "Run calibrate again.",
                 (unsigned)item.len, (unsigned)cal.version);
        return;
    }
    /* A calibration that says the sheet has no extent is not a calibration,
     * and one the controller could not have produced is arithmetic rather than
     * measurement. Either way the compiled bounds are the better answer: they
     * are somebody else's measurements, which beats impossible ones.
     *
     * The second case is not hypothetical. Until #149 the calibration scaled
     * each channel by the other axis's ratio, which over-corrected one of them
     * past the end of the converter's range -- a bench board had y running
     * from -56 to 4175 on a device that can only read 0 to 4095. Anybody who
     * calibrated before that fix has such a mapping saved, and an upgrade that
     * kept it would look exactly like the bug never being fixed. */
    observore_touchcal_t fit = {cal.lo_x, cal.hi_x, cal.lo_y, cal.hi_y};
    if (!observore_touchcal_fit(&fit, RAW_MAX)) {
        ESP_LOGW(TAG, "stored calibration is not usable (x %d-%d, y %d-%d); "
                      "using the built-in one -- run calibrate again",
                 cal.lo_x, cal.hi_x, cal.lo_y, cal.hi_y);
        return;
    }
    s_lo_x = fit.lo_x; s_hi_x = fit.hi_x;
    s_lo_y = fit.lo_y; s_hi_y = fit.hi_y;
    s_calibrated = true;
    ESP_LOGI(TAG, "calibrated here: x %d-%d, y %d-%d",
             s_lo_x, s_hi_x, s_lo_y, s_hi_y);
}

void observore_touch_set_bounds(int min_x, int max_x, int min_y, int max_y)
{
    /* The same judgement as on load, on the way in: nothing unusable should
     * reach the flash in the first place, and a fresh calibration from a
     * slightly non-linear panel gets clamped rather than refused. */
    observore_touchcal_t fit = {min_x, max_x, min_y, max_y};
    if (!observore_touchcal_fit(&fit, RAW_MAX)) {
        ESP_LOGW(TAG, "refusing a calibration the controller could not have "
                      "produced: x %d-%d, y %d-%d", min_x, max_x, min_y, max_y);
        return;
    }
    min_x = fit.lo_x; max_x = fit.hi_x;
    min_y = fit.lo_y; max_y = fit.hi_y;

    s_lo_x = min_x; s_hi_x = max_x;
    s_lo_y = min_y; s_hi_y = max_y;
    s_calibrated = true;

    touch_cal_t cal = {TOUCH_CAL_VERSION,
                       (int16_t)min_x, (int16_t)max_x,
                       (int16_t)min_y, (int16_t)max_y};
    observore_nvs_item_t item = {.key = "touchcal", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &cal, .len = sizeof(cal)};
    if (observore_nvs_write(&item, 1) == ESP_OK) {
        ESP_LOGI(TAG, "calibration saved: x %d-%d, y %d-%d",
                 min_x, max_x, min_y, max_y);
    } else {
        ESP_LOGW(TAG, "calibration applied but not saved");
    }
}

void observore_touch_bounds(int *min_x, int *max_x, int *min_y, int *max_y)
{
    if (min_x) { *min_x = s_lo_x; }
    if (max_x) { *max_x = s_hi_x; }
    if (min_y) { *min_y = s_lo_y; }
    if (max_y) { *max_y = s_hi_y; }
}

bool observore_touch_calibrated(void) { return s_calibrated; }

static void to_screen(int raw_x, int raw_y, int *x, int *y)
{
    int lo_x = s_lo_x, hi_x = s_hi_x;
    int lo_y = s_lo_y, hi_y = s_hi_y;

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

#if CONFIG_OBSERVORE_TOUCH_XPT2046

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

static bool backend_init(void)
{
#if !CONFIG_OBSERVORE_TOUCH_POLLED
    /* PENIRQ: low while the panel is touched. Polling this costs one register
     * read, so the SPI bus stays idle until there is something to read. */
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << CONFIG_OBSERVORE_TOUCH_IRQ,
        .mode         = GPIO_MODE_INPUT,
    };
    gpio_config(&irq);
#endif

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
        return false;
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
        return false;
    }
#if CONFIG_OBSERVORE_TOUCH_POLLED
    ESP_LOGI(TAG, "XPT2046 on SPI%d (polled, no irq line)%s", TOUCH_HOST + 1,
             CONFIG_OBSERVORE_TOUCH_SHARED_BUS ? ", sharing the panel's bus" : "");
#else
    ESP_LOGI(TAG, "XPT2046 on SPI%d (irq %d)%s", TOUCH_HOST + 1,
             CONFIG_OBSERVORE_TOUCH_IRQ,
             CONFIG_OBSERVORE_TOUCH_SHARED_BUS ? ", sharing the panel's bus" : "");
#endif
    return true;
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
#if CONFIG_OBSERVORE_TOUCH_POLLED
        int lvl = -1;
#else
        int lvl = gpio_get_level(CONFIG_OBSERVORE_TOUCH_IRQ);
#endif
        int z   = s_ready ? pressure() : -1;
        int rx  = s_ready ? median_channel(CMD_X)  : -1;
        int ry  = s_ready ? median_channel(CMD_Y)  : -1;
        ESP_LOGI(TAG, "probe irq=%d z=%4d x=%4d y=%4d", lvl, z, rx, ry);
    }
#endif
#if CONFIG_OBSERVORE_TOUCH_POLLED
    /* No interrupt line on this board, so there is nothing to ask before
     * reading. The pressure threshold decides, which is what it is for; the
     * gate was only ever an optimisation. */
    if (!s_ready) {
        return false;
    }
#else
    if (!s_ready || gpio_get_level(CONFIG_OBSERVORE_TOUCH_IRQ) != 0) {
        return false;
    }
#endif
    /* The backlight is a PWM whose switching sits inside the controller's
     * measuring band, so it is held steady across a measurement. Whether it
     * actually matters could not be settled on the bench: the panel there lost
     * one of its two plates partway through bring-up, which masks any smaller
     * effect. It costs about a millisecond, inside the interrupt gate, so it
     * only ever happens with a finger already on the glass. */
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

#else  /* CONFIG_OBSERVORE_TOUCH_CAPACITIVE */

/* The capacitive half.
 *
 * Most of what the resistive half does is unnecessary here: the controller
 * reports how many fingers are down and where each one is, so there is no
 * pressure to threshold and no median to take.
 *
 * What it does NOT remove is the mapping. This comment used to go on to say
 * there was "no calibration to get wrong -- the arithmetic that took two days
 * and a wrongly accused ribbon cable on the Cheap Yellow Displays does not
 * exist here", and that was wrong in the way a confident comment is worst:
 * it discouraged looking. A capacitive controller reports in the range its
 * firmware was configured for, which is usually the panel it shipped against
 * and is not guaranteed to be this one. So the reading goes through the same
 * to_screen() as the resistive panels, with bounds that can be measured on
 * the glass rather than assumed. */

/* Registers, from Waveshare's own drivers rather than from a datasheet
 * nobody has: 0x02 holds the number of fingers down, and 0x03 begins four
 * bytes of position -- twelve bits of X then twelve of Y, high nibble first.
 *
 * The FT3168 on the 1.43 and the CST816 on the 1.85 agree on all of that,
 * which is why one backend reads both. Where they differ is where they live:
 * the CST816 is on a bus of its own and its reset is on the I/O expander
 * rather than a pin. */
#define FT_REG_TOUCHES 0x02
#define FT_REG_XY      0x03

#if CONFIG_OBSERVORE_TOUCH_ON_AUX_BUS
#define TOUCH_BUS OBSERVORE_I2C_AUX
#else
#define TOUCH_BUS OBSERVORE_I2C_MAIN
#endif

static i2c_master_dev_handle_t s_touch;

static bool ft_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return observore_i2c_read(s_touch, reg, buf, len);
}

static bool backend_init(void)
{
    if (!observore_i2c_bus_of(TOUCH_BUS)) {
        ESP_LOGW(TAG, "no I2C bus for the touch panel");
        return false;
    }

#if CONFIG_OBSERVORE_TOUCH_RST_IOEXP_BIT >= 0
    /* Held in reset until something lets go of it. The expander has to be up
     * first, which it is: it comes up with the clock's bus, well before any
     * of this. Ten milliseconds low and fifty to settle, which is what the
     * vendor's own driver waits. */
    if (!observore_ioexp_reset_pulse(CONFIG_OBSERVORE_TOUCH_RST_IOEXP_BIT,
                                     10, 50)) {
        ESP_LOGW(TAG, "could not pulse the touch reset on expander line %d",
                 CONFIG_OBSERVORE_TOUCH_RST_IOEXP_BIT);
    }
#endif

    /* Which chips answered, said out loud: "the panel is not answering" and
     * "the panel is not there" look identical in a log mentioning neither.
     * On the 1.43 three chips share these wires; on the 1.85 the touch
     * controller has them to itself and the others are on the main bus. */
    char found[64];
    observore_i2c_scan_on(TOUCH_BUS, found, sizeof(found));
    ESP_LOGI(TAG, "touch I2C on sda %d / scl %d answered:%s",
             CONFIG_OBSERVORE_TOUCH_SDA, CONFIG_OBSERVORE_TOUCH_SCL,
             found[0] ? found : " nothing");

    s_touch = observore_i2c_device_on(TOUCH_BUS,
                                      CONFIG_OBSERVORE_TOUCH_I2C_ADDR,
                                      300 * 1000);
    if (!s_touch) {
        return false;
    }

#if CONFIG_OBSERVORE_TOUCH_FT3168
    /* Out of whatever low-power mode it woke in, the way the vendor does it.
     * Only the FT3168: on the CST816 register 0x00 is not a mode register
     * and writing to it is not something its vendor driver does. */
    observore_i2c_write(s_touch, 0x00, 0x00);
    ESP_LOGI(TAG, "FT3168 at 0x%02X", CONFIG_OBSERVORE_TOUCH_I2C_ADDR);
#else
    /* The chip says what it is, which is worth asking once: a panel that
     * answers its address but reports a chip id of zero is a panel still in
     * reset, and that reads identically to one nobody is touching. */
    uint8_t id[3] = {0};
    if (observore_i2c_read(s_touch, 0xA7, id, sizeof(id))) {
        ESP_LOGI(TAG, "CST816 at 0x%02X, chip 0x%02X proj 0x%02X fw 0x%02X",
                 CONFIG_OBSERVORE_TOUCH_I2C_ADDR, id[0], id[1], id[2]);
    } else {
        ESP_LOGW(TAG, "CST816 at 0x%02X would not say what it is",
                 CONFIG_OBSERVORE_TOUCH_I2C_ADDR);
    }

    /* Two registers the vendor's own driver writes and this code did not.
     *
     * 0xFA enables the motion interrupts, which is how the chip is meant to
     * be told that somebody will be reading it.
     *
     * 0xFE disables auto-sleep, and it is why two polls in three were refused
     * on an idle panel here. The chip powers its sensing down between touches
     * and declines I2C while it is down; the reads that failed were not a
     * wiring fault, a clock-speed fault or a pull-up fault, which is where
     * this would otherwise have been looked for. It costs a little current to
     * keep it awake, and this board is the one with eight megabytes of PSRAM
     * and a battery gauge rather than the one counting microamps. */
    observore_i2c_write(s_touch, 0xFA, 0x70);
    observore_i2c_write(s_touch, 0xFE, 0x01);
#endif
    return true;
}

static bool sample_raw(int *x, int *y)
{
    /* Registers 0x00 to 0x06 in one transaction, rather than reading the
     * finger count and then the position separately.
     *
     * Two reasons, and the second is the one that bites. A second transaction
     * is a second chance to be refused by a chip that sleeps between polls --
     * and measured on the 1.85 while idle, two reads in three were refused.
     * And the count and the coordinates came from different moments: the
     * finger moves, and the pair could disagree about whether it was even
     * still down. One read is one snapshot. It is also what the reference
     * driver for this chip does. */
    uint8_t reg[7] = {0};
    bool got = ft_read(0x00, reg, sizeof(reg));
    uint8_t touches = got ? (uint8_t)(reg[FT_REG_TOUCHES] & 0x03) : 0;
#if CONFIG_OBSERVORE_TOUCH_LOG_RAW
    /* Bring-up: "nobody is touching it" and "the read failed" are the same
     * silence otherwise, and telling them apart is most of the work.
     *
     * Worth knowing before reaching for either: this glass is capacitive, so
     * a passive plastic stylus registers as nothing at all, correctly and
     * for ever. Twenty-five taps of one went entirely unseen here while the
     * driver reported reads succeeding and no fingers down. */
    static int64_t s_last_probe_us;
    int64_t now_probe = esp_timer_get_time();
    if (now_probe - s_last_probe_us > 2 * 1000 * 1000) {
        s_last_probe_us = now_probe;
        ESP_LOGI(TAG, "probe read=%s fingers=%u", got ? "ok" : "failed", touches);
    }
#endif
    if (!got || touches == 0) {
        return false;
    }
    const uint8_t *buf = &reg[FT_REG_XY];
    int raw_x = ((buf[0] & 0x0F) << 8) | buf[1];
    int raw_y = ((buf[2] & 0x0F) << 8) | buf[3];

    /* Checked against what the register can hold, not against the size of the
     * panel.
     *
     * It used to reject anything at or past OBSERVORE_DISPLAY_W, on the
     * assumption that a reading is a panel pixel. For a controller configured
     * for a *larger* range than its panel that silently kills the far edges:
     * the bottom and right of the glass report numbers above the panel size,
     * every one of them is discarded, and the symptom is a strip of screen
     * where nothing responds -- with no log line, because the discard came
     * before the logging. Twelve bits is what these registers are. */
    bool over = raw_x > RAW_MAX || raw_y > RAW_MAX;
#if CONFIG_OBSERVORE_TOUCH_LOG_RAW
    /* Logged before the check rather than after it, which is the whole point:
     * a range wider than expected shows up as readings that are *dropped*,
     * and a log of accepted readings only is blind to exactly that. */
    ESP_LOGI(TAG, "touch n=%u raw x=%4d y=%4d%s", touches, raw_x, raw_y,
             over ? "  DROPPED (not a 12-bit reading)" : "");
#endif
    if (over) {
        return false;
    }
    *x = raw_x;
    *y = raw_y;
    return true;
}

/* And the same reading mapped to the screen, which is what the poll task
 * wants. Split from the above so the calibration can see the controller's own
 * numbers: calibrating from already-mapped values would solve for the bounds
 * the mapping is currently using and report success having changed nothing. */
static bool sample(int *x, int *y)
{
    int raw_x = 0, raw_y = 0;
    if (!sample_raw(&raw_x, &raw_y)) {
        return false;
    }
    to_screen(raw_x, raw_y, x, y);
    return true;
}

#endif  /* controller */

void observore_touch_init(void)
{
    if (!backend_init()) {
        return;
    }
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    /* Only a resistive sheet has edges to learn. A capacitive controller
     * reports panel pixels and has nothing to calibrate. */
    cal_load();
#endif
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
    s_task_started_us = esp_timer_get_time();
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
        int tx = 0, ty = 0;
        if (observore_tap_sample(&s_tap, down, sx, sy, now, RELEASE_US,
                                 &tx, &ty)) {
            s_tap_pending = true;
            s_tap_x = tx;
            s_tap_y = ty;
        }
        s_down = s_tap.down;
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

bool observore_touch_raw(int *x, int *y)
{
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    if (!s_ready) {
        return false;
    }
    observore_display_backlight_hold();
    int z = pressure();
    int rx = z >= Z_THRESHOLD ? median_channel(CMD_X) : -1;
    int ry = z >= Z_THRESHOLD ? median_channel(CMD_Y) : -1;
    observore_display_backlight_release();
    if (z < Z_THRESHOLD || rx < 0 || ry < 0) {
        return false;
    }
    if (x) { *x = rx; }
    if (y) { *y = ry; }
    return true;
#else
    /* The capacitive controller's own numbers, unmapped.
     *
     * This used to return false with a comment saying "a capacitive
     * controller reports panel pixels, so there is nothing to calibrate and
     * nothing raw to report". That assumption appeared in three places --
     * here, the mapping, and the settings page that offers the calibration --
     * and all three had to be wrong together for the round panels to have no
     * way of being corrected. */
    if (!s_ready) {
        return false;
    }
    observore_display_backlight_hold();
    int rx = 0, ry = 0;
    bool got = sample_raw(&rx, &ry);
    observore_display_backlight_release();
    if (!got) {
        return false;
    }
    if (x) { *x = rx; }
    if (y) { *y = ry; }
    return true;
#endif
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
bool observore_touch_raw(int *x, int *y)  { (void)x; (void)y; return false; }
void observore_touch_set_bounds(int a, int b, int c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
}
void observore_touch_bounds(int *a, int *b, int *c, int *d)
{
    if (a) { *a = 0; } if (b) { *b = 0; }
    if (c) { *c = 0; } if (d) { *d = 0; }
}
bool observore_touch_calibrated(void) { return false; }

#endif
