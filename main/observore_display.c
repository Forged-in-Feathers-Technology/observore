#include "observore_display.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_DISPLAY

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_app_desc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_OBSERVORE_DISPLAY_ILI9341
#include "esp_lcd_ili9341.h"
#define PANEL_NAME "ILI9341"
#define new_panel  esp_lcd_new_panel_ili9341
#elif CONFIG_OBSERVORE_DISPLAY_ST7796
#include "esp_lcd_st7796.h"
#define PANEL_NAME "ST7796"
#define new_panel  esp_lcd_new_panel_st7796
#elif CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
#include "esp_lcd_sh8601.h"
#if CONFIG_OBSERVORE_DISPLAY_CO5300
#define PANEL_NAME "CO5300"
#else
#define PANEL_NAME "SH8601"
#endif
#define new_panel  esp_lcd_new_panel_sh8601
#else
#include "esp_lcd_panel_st7789.h"
#define PANEL_NAME "ST7789"
#define new_panel  esp_lcd_new_panel_st7789
#endif
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "observore_detect.h"
#include "observore_font.h"
#include <limits.h>
#include <time.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "observore_battery.h"
#include "observore_clock.h"
#include "observore_watch.h"
#include "observore_motion.h"
#include "observore_mute.h"
#include "observore_netcfg.h"
#include "observore_nvs.h"
#include "observore_runs.h"
#include "observore_wifi.h"
#include "observore_touchcal.h"
#include "observore_touch.h"
#include "observore_update.h"
#include "observore_util.h"

/* Bool options that are off are undefined, not zero, so give the ones read
 * as values a definite one. */
#ifndef CONFIG_OBSERVORE_DISPLAY_BGR
#define CONFIG_OBSERVORE_DISPLAY_BGR 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_INVERT
#define CONFIG_OBSERVORE_DISPLAY_INVERT 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_MIRROR_X
#define CONFIG_OBSERVORE_DISPLAY_MIRROR_X 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_MIRROR_Y
#define CONFIG_OBSERVORE_DISPLAY_MIRROR_Y 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_X_OFFSET
#define CONFIG_OBSERVORE_DISPLAY_X_OFFSET 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_INSET_X
#define CONFIG_OBSERVORE_DISPLAY_INSET_X 0
#endif
#ifndef CONFIG_OBSERVORE_DISPLAY_INSET_Y
#define CONFIG_OBSERVORE_DISPLAY_INSET_Y 0
#endif

static const char *TAG = "observore.display";

/* Landscape. The panel is 240x320 portrait; swapping axes gives 320 wide. */
#define DISP_W OBSERVORE_DISPLAY_W
#define DISP_H OBSERVORE_DISPLAY_H

/* Where the text grid begins, and how much of the glass it may use.
 *
 * Zero on a rectangular panel, so the grid is the screen. On a round one the
 * grid is the largest square that fits inside the circle -- 41 columns by 20
 * rows on a 466-pixel one -- and the inset is the quarter-circle at each
 * corner that no character may be drawn into. Text does not reflow around a
 * curve legibly at eight pixels a character, and a page of readings that is
 * clipped at the corners is worse than one that is smaller and whole. */
#define INSET_X CONFIG_OBSERVORE_DISPLAY_INSET_X
#define INSET_Y CONFIG_OBSERVORE_DISPLAY_INSET_Y
#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
#define X_OFF   s_x_off
#else
#define X_OFF   CONFIG_OBSERVORE_DISPLAY_X_OFFSET
#endif
#define COLS   ((DISP_W - 2 * INSET_X) / OBSERVORE_FONT_W)   /* 40 */
#define ROWS   ((DISP_H - 2 * INSET_Y) / OBSERVORE_FONT_H)   /* 15 */

/* RGB565. Chosen to read across a room, not to be pretty. */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
/* Brighter than C_GREY, for a dial rim that has to read as an edge rather
 * than as a smudge: 0x8410 at this radius looked like a fault. */
#define C_SILVER 0xC618
#define C_GREEN  0x07E0
#define C_AMBER  0xFD20
#define C_RED    0xF800
/* Just off black: enough to read the button bar as a raised strip rather than
 * as part of the page above it. */
#define C_DARK   0x2104

/* Rows the pages must leave alone: the bar when there is touch, one footer
 * row when there is not. */
#if CONFIG_OBSERVORE_TOUCH
#define BAR_LAST 2
#else
#define BAR_LAST 1
#endif

/* The bar is drawn two rows deep and answers to the bottom three.
 *
 * A single text row is sixteen pixels, about two millimetres: smaller than a
 * fingertip, at the edge of the glass where a resistive sheet is least
 * accurate, and on this board partly under the lip of a case. The margin above
 * the drawn bar is what makes it hittable without looking. */
#define BAR_ROWS 2
#define BAR_TOUCH_ROWS 3


/* One DMA-capable strip, shared by everything that writes a block of pixels.
 *
 * Allocated once at startup rather than per call. Two reasons: a fifteen
 * kilobyte internal allocation is a large ask on a board whose Wi-Fi buffers
 * have just moved to PSRAM -- the largest free internal block is under twelve
 * kilobytes once it is running, so the old per-call malloc would now fail --
 * and a buffer that is taken and released repeatedly is a buffer that
 * fragments the heap it lives in. */
static uint16_t *s_strip;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
#if CONFIG_OBSERVORE_WATCHFACE
/* Posted when the panel has finished with a buffer.
 *
 * esp_lcd_panel_draw_bitmap queues the transfer and returns; the DMA reads
 * the buffer afterwards. Copying the next strip into the same buffer without
 * waiting overwrites data still in flight, which is why parts of the watch
 * face never arrived and the previous page showed through them in cyan. */
static SemaphoreHandle_t s_blit_done;

static bool blit_done(esp_lcd_panel_io_handle_t io,
                      esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev; (void)ctx;
    BaseType_t woken = pdFALSE;
    if (s_blit_done) {
        xSemaphoreGiveFromISR(s_blit_done, &woken);
    }
    return woken == pdTRUE;
}
#endif
/* What is on the glass, so a redraw sends only the lines that changed. */
static char     s_shown[ROWS][COLS + 1];
static uint16_t s_shown_bg[ROWS];
static bool     s_ready;
static char     s_notice[COLS + 1];
static int64_t  s_notice_until_us;

/* A whole 320x240 framebuffer is 150 KB, which this board does not have, so
 * drawing is done a glyph at a time through 256-byte cells. Slow in the
 * abstract, fine for a status screen that changes every few seconds.
 *
 * A ring of them, not one. draw_bitmap queues the transfer and returns before
 * DMA has read the buffer, so writing the next glyph into the same cell
 * corrupts the one in flight. The first light-up showed exactly that: solid
 * cells looked fine, text was gibberish. The queue holds four transfers, so
 * eight cells means the one being reused finished at least four draws ago. */
#define CELLS 8
static uint16_t s_cells[CELLS][OBSERVORE_FONT_W * OBSERVORE_FONT_H];
static unsigned s_cell_next;

#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
/* Waveshare's own sequence for each controller, kept as they wrote it.
 *
 * 0x11 sleep out, 0x53 brightness control on, 0x51 brightness (starting at
 * zero so nothing is watched initialising), 0x29 display on, then 0x51 again
 * at full. The CO5300 wants 0xC4 set first and no tearing-effect line; the
 * SH8601 wants the TE configuration and no 0xC4. Neither list is guessed. */
static const sh8601_lcd_init_cmd_t CO5300_INIT[] = {
    {0x11, (uint8_t []){0x00}, 0, 80},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};
static const sh8601_lcd_init_cmd_t SH8601_INIT[] = {
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

/* What register 0xDA answers. 0xFF is also what a floating line reads as,
 * which is why it is the fallback rather than the certain case: the vendor
 * treats anything that is not an SH8601 as a CO5300, and so does this. */
#define PANEL_ID_SH8601 0x86

/* The CO5300 addresses a frame six pixels wider than the glass and starts it
 * six along; the SH8601 does not. Decided with the controller, so it cannot
 * disagree with the initialisation sequence. */
static int s_x_off = CONFIG_OBSERVORE_DISPLAY_X_OFFSET;

/* Brightness is a command here rather than a pin, so the levels are the
 * panel's own 0-255 rather than a PWM duty. Same four steps as everywhere
 * else, and they mean the same thing to a reader. */
#define PANEL_BRIGHTNESS_CMD 0x51

#if CONFIG_OBSERVORE_DISPLAY_PANEL_DETECT
/* Ask the panel what it is, before the SPI driver owns its pins.
 *
 * One product ships with either controller, which is fine for a board on a
 * bench and not fine for a download: a stranger cannot be expected to know
 * which revision arrived in the post, and the failure -- a blank screen, or
 * an image six pixels sideways -- tells them nothing at all.
 *
 * So the pins are driven by hand for a moment. The panel understands a
 * one-line SPI at reset regardless of which controller it is: a 0x03 read
 * command, the register, then eight clocks with D0 turned around. This is
 * Waveshare's own sequence, kept deliberately close to their code, timing
 * and all -- it runs once, at boot, and costs half a second. */
#define PIN_CS   CONFIG_OBSERVORE_DISPLAY_CS
#define PIN_CLK  CONFIG_OBSERVORE_DISPLAY_SCLK
#define PIN_D0   CONFIG_OBSERVORE_DISPLAY_QSPI_D0
#define PIN_RST  CONFIG_OBSERVORE_DISPLAY_RST

static void bb_send(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_level(PIN_D0, (v & 0x80) ? 1 : 0);
        v = (uint8_t)(v << 1);
        gpio_set_level(PIN_CLK, 0);
        gpio_set_level(PIN_CLK, 1);
    }
}

static void bb_d0_input(bool in)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_D0,
        .mode         = in ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
}

static uint8_t panel_id(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_CLK) |
                        (1ULL << PIN_D0) | (1ULL << PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return 0;
    }
    /* The reset the controller needs before it will answer anything. */
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    bb_send(0x03);            /* read */
    bb_send(0x00);
    bb_send(0xDA);            /* the identifier */
    bb_send(0x00);

    uint8_t id = 0;
    for (int i = 0; i < 8; i++) {
        gpio_set_level(PIN_CLK, 0);
        bb_d0_input(true);
        esp_rom_delay_us(1);
        id = (uint8_t)((id << 1) | (gpio_get_level(PIN_D0) & 1));
        bb_d0_input(false);
        gpio_set_level(PIN_CLK, 1);
        esp_rom_delay_us(1);
    }
    gpio_set_level(PIN_CS, 1);

    /* Hand the pins back, so the SPI driver configures them from scratch. */
    gpio_reset_pin(PIN_CS);
    gpio_reset_pin(PIN_CLK);
    gpio_reset_pin(PIN_D0);
    return id;
}
#endif  /* PANEL_DETECT */
#endif  /* QSPI_AMOLED */

/* One row of glyph cells, which is how much of the panel is cleared at a
 * time. A round panel has corners the text grid never reaches, and an AMOLED
 * powers up with whatever was last in its RAM, so the whole surface is
 * written once at startup rather than only the part with characters on it. */
#define CLEAR_STRIP_BYTES (DISP_W * OBSERVORE_FONT_H * 2)

/* The panel takes each 16-bit pixel most significant byte first and the SPI
 * path sends the host's bytes as they are, so the swap is done here. Measured
 * on the bench rather than taken from the data_endian field: without this the
 * green band came out red, and with inversion on top it came out cyan. */
#if CONFIG_OBSERVORE_DISPLAY_SWAP_BYTES
#if CONFIG_OBSERVORE_DISPLAY_SWAP_BYTES
static inline uint16_t px(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
#else
static inline uint16_t px(uint16_t c) { return c; }
#endif
#else
static inline uint16_t px(uint16_t c) { return c; }
#endif

/* Black the whole panel, corners included, in strips one glyph tall. Uses a
 * temporary buffer rather than a static one: this runs once, and on the board
 * that needs it there are eight megabytes of PSRAM to borrow it from. */
#if CONFIG_OBSERVORE_TOUCH
/* Panel pixels to text-grid pixels.
 *
 * The two are the same thing on a rectangular panel. On a round one the grid
 * is the square inside the circle, which leaves four crescents of glass that
 * no character is drawn into -- and a tap there is clamped to the nearest
 * cell rather than discarded.
 *
 * Discarding was the first attempt and it was wrong, for a reason that only
 * a finger shows: the crescents are not bezel, they are live glass, and the
 * button bar sits along the bottom edge of the square with seventy pixels of
 * touchable nothing beneath it. Presses aimed at the bar landed at y=403 to
 * 424 against a grid ending at 393 -- below the words, on the glass, ignored.
 * Ten of twelve deliberate presses did nothing at all.
 *
 * There is no bezel to rest a thumb on here, so the objection that made
 * rejecting look careful does not apply to this shape. */
static bool to_grid_px(int *x, int *y)
{
    int gx = *x - INSET_X;
    int gy = *y - INSET_Y;
    const int max_x = COLS * OBSERVORE_FONT_W - 1;
    const int max_y = ROWS * OBSERVORE_FONT_H - 1;
    *x = gx < 0 ? 0 : (gx > max_x ? max_x : gx);
    *y = gy < 0 ? 0 : (gy > max_y ? max_y : gy);
    return true;
}
#endif

static void clear_panel(void)
{
    uint16_t *strip = s_strip;
    if (!strip) {
        ESP_LOGW(TAG, "no buffer to clear the panel with; corners may be stale");
        return;
    }
    memset(strip, 0, CLEAR_STRIP_BYTES);
    for (int y = 0; y < DISP_H; y += OBSERVORE_FONT_H) {
        int h = (y + OBSERVORE_FONT_H <= DISP_H) ? OBSERVORE_FONT_H : DISP_H - y;
        esp_lcd_panel_draw_bitmap(s_panel, X_OFF, y, X_OFF + DISP_W, y + h, strip);
    }
}

static void draw_glyph(int col, int row, char c, uint16_t fg, uint16_t bg)
{
    if (c < OBSERVORE_FONT_FIRST || c > OBSERVORE_FONT_LAST) {
        c = '?';
    }
    const uint8_t *g = OBSERVORE_FONT[c - OBSERVORE_FONT_FIRST];
    uint16_t *cell = s_cells[s_cell_next++ % CELLS];
    for (int y = 0; y < OBSERVORE_FONT_H; y++) {
        for (int x = 0; x < OBSERVORE_FONT_W; x++) {
            cell[y * OBSERVORE_FONT_W + x] = px((g[y] >> (7 - x)) & 1 ? fg : bg);
        }
    }
    int x0 = X_OFF + INSET_X + col * OBSERVORE_FONT_W;
    int y0 = INSET_Y + row * OBSERVORE_FONT_H;
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + OBSERVORE_FONT_W,
                              y0 + OBSERVORE_FONT_H, cell);
}

/* Write one full row, padded to the width so the previous content is covered.
 * Skipped when the row already shows exactly this. */
static void line(int row, const char *text, uint16_t fg, uint16_t bg)
{
    char buf[COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", COLS, text ? text : "");
    if (strcmp(buf, s_shown[row]) == 0 && s_shown_bg[row] == bg) {
        return;
    }
    for (int col = 0; col < COLS; col++) {
        draw_glyph(col, row, buf[col], fg, bg);
    }
    memcpy(s_shown[row], buf, sizeof(buf));
    s_shown_bg[row] = bg;
}

/* What the main loop last published, and what the drawing task renders from.
 *
 * The two are separated because they run at different rates for different
 * reasons. The main loop produces a status once per patrol heartbeat and
 * spends about thirty seconds of every cycle inside a blocking scan; a screen
 * that only redrew there would ignore a finger for half a minute. The drawing
 * task redraws on its own clock, from the last thing published. */
#define UI_POLL_MS   30
/* The drawing task's stack. v0.8.3 trimmed it to 2 KB, because a board with no
 * PSRAM needs every spare kilobyte for the TLS handshake behind its update check
 * -- a too-generous stack here is what left v0.8.2 unable to see its own updates.
 * But the on-screen keyboard added since draws deeper than the plain status
 * lines: 2560 was measured to leave only 536 bytes spare, so it is back at 3 KB.
 * The extra ~1 KB is affordable -- the CYD's uplink low-water still sits near
 * 27 KB, well clear of the handshake. */
#define UI_STACK     3072
/* How many devices one page of the snapshot holds. */
#define SNAP_MAX     8

static SemaphoreHandle_t s_lock;
static observore_status_t s_snap_st;
static observore_event_t  s_snap_top[SNAP_MAX];
static size_t             s_snap_n;
static int64_t            s_snap_now_us;
static bool               s_have_snap;
static bool               s_dirty;

/* Which page, and which button is showing as pressed. */
typedef enum {
#if CONFIG_OBSERVORE_WATCHFACE
               /* First, so a glance shows a watch rather than a list. */
               PAGE_CLOCK = 0,
#endif
               PAGE_WATCH,  PAGE_SYSTEM,
#if CONFIG_OBSERVORE_TOUCH
               PAGE_WIFI,
#endif
               PAGE_COUNT } page_t;
static page_t  s_page;
static int     s_pressed = -1;        /* button index, or -1 */
static int64_t s_pressed_until_us;
static bool    s_baseline_request;

#if CONFIG_OBSERVORE_TOUCH
/* Actions that change something get a second tap to confirm, the way the
 * console's install and baseline buttons do: a resistive panel in a pocket or
 * under a sleeve can register a press nobody meant. The arming lapses on its
 * own, so a forgotten half-press does nothing. */
#define ARM_TIMEOUT_US (5 * 1000000)
static int     s_armed_row = -1;      /* watch page: the finding being ignored */
/* How many taps the glass has reported since boot, phantom or otherwise. */
static uint32_t s_taps;
static int64_t s_armed_until_us;
/* The baseline button, armed the same way.
 *
 * It was one tap, acting immediately, on the most destructive thing this
 * device can do: a baseline silences everything in range at once, and a
 * mistaken one can hide exactly what the device exists to find. Dismissing a
 * single finding already needed two taps, and the console already asks for
 * confirmation -- the glass was the one path with no guard, on the action
 * that deserved it most.
 *
 * Hit twice by accident in one day on a board with no case yet, the second
 * time as its owner left an office, which silenced the population there and
 * quietly invalidated the journey it was carried on. */
static int64_t s_baseline_armed_until_us;
/* Clearing every ignore rule, armed the same way.
 *
 * Undoing a baseline needed a console, and the board that most needed it --
 * a screen in a pocket with no network configured -- had none. Getting out of
 * an accidental baseline took flashing a one-shot firmware twice. The device
 * that can silence a room with one tap should be able to unsilence it from
 * the same glass. */
static int64_t s_clear_armed_until_us;
/* Calibrating this panel's own sheet.
 *
 * -1 when not calibrating; otherwise which of the two targets is showing.
 * Two opposite corners are enough for a linear map, and asking for four
 * would mostly be asking somebody to press the two corners that sit under
 * the bezel on these boards. */
static int   s_cal_step = -1;
static int   s_cal_raw_x[2], s_cal_raw_y[2];
static int64_t s_cal_ready_us;
static bool    s_install_armed;
static int64_t s_install_armed_until_us;
#endif

#if CONFIG_OBSERVORE_TOUCH
/* The network page is a small state machine: a list of what the last patrol
 * scan saw, then a keyboard for the one that was chosen.
 *
 * The password is typed blind. The key you press is shown, because a keyboard
 * that does not say what it registered is unusable on a resistive panel, but
 * the field itself only ever shows dots. A screen faces a room, and the whole
 * reason this device has no password on its glass is that someone else may be
 * in it. Blind entry is the same rule applied to typing. */
typedef enum { WIFI_LIST = 0, WIFI_TYPING, WIFI_SAVED } wifi_step_t;

#define KEY_ROWS 4
#define KEY_COLS 13
/* Four layers, each thirteen keys by four rows; a blank is a gap. Lower and
 * upper are the same letters because a Wi-Fi password is case-sensitive and a
 * person needs to be able to type either without hunting. */
static const char *const KEYS[2][KEY_ROWS] = {
    {"1234567890   ",
     "qwertyuiop   ",
     "asdfghjkl    ",
     "zxcvbnm      "},
    {"!@#$%^&*()_-+",
     "QWERTYUIOP{}",
     "ASDFGHJKL:;'",
     "ZXCVBNM,.?/  "},
};

/* Which finding each screen row is showing, for tap-to-ignore. */
static int s_row_finding[ROWS];

static wifi_step_t s_wifi_step;
static observore_scan_entry_t s_aps[10];
static size_t s_ap_count;
static int    s_ap_chosen = -1;
static char   s_pass[OBSERVORE_PASSWORD_LEN];
static size_t s_pass_len;
static bool   s_shift;
/* Reveal is deliberate, momentary and off by default. Blind entry is the rule
 * -- a screen faces a room -- but a resistive panel and a fingertip make a
 * wrong key easy and invisible, and a person who cannot check what they typed
 * will simply get it wrong repeatedly. So the choice is theirs, for a few
 * seconds at a time, rather than mine forever. */
static bool    s_reveal;
static int64_t s_reveal_until_us;
static char   s_wifi_msg[41];

/* The typing view, laid out against the fact that the button bar owns the
 * last two rows and answers to the last three. Everything here must sit above
 * row ROWS - BAR_TOUCH_ROWS or it is drawn over and cannot be pressed --
 * which is exactly what happened to the first version of this page. */
#define KB_HEIGHT  2
/* Anchored to the bottom rather than the top, so the keyboard sits against the
 * button bar on a screen of any height: forty by fifteen on the 2.8" boards,
 * sixty by twenty on the 3.5". The action row is the last one clear of the
 * bar's touch zone. */
/* The action strip gets two rows and a blank one below it wherever the screen
 * can spare them, which the 3.5" boards can and the 2.8" ones cannot.
 *
 * One row is sixteen pixels. The strip sits directly above the button bar, so
 * on a single row a press a couple of pixels low crosses into it: ABC# and
 * page answered each other's presses until the strip got a margin. */
#define ACTION_ROWS ((ROWS >= 18) ? 2 : 1)
#define ACTION_GAP  ((ROWS >= 18) ? 1 : 0)
#define ACTION_TOP  (ROWS - BAR_TOUCH_ROWS - ACTION_GAP - ACTION_ROWS)
#define ACTION_ROW  (ACTION_TOP + ACTION_ROWS - 1)   /* where the labels sit */
#define KB_TOP      (ACTION_TOP - KEY_ROWS * KB_HEIGHT)
/* Keys share the width: three columns each at forty, four at sixty. */
#define KEY_W      (COLS / KEY_COLS)
#endif

/* Backlight, as a duty cycle rather than on/off. A 2.8" panel at full
 * brightness is a beacon in a dark room, which is the wrong thing for this
 * device to be; and the level is remembered, because a detector that comes
 * back from a power cut at full brightness at three in the morning has told
 * the room something. */
#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MODE    LEDC_LOW_SPEED_MODE
#define BL_BITS    LEDC_TIMER_8_BIT
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define BL_COUNT OBSERVORE_BRIGHT_STEPS
static const uint8_t BL_LEVELS[BL_COUNT] = {255, 160, 80, 24};
static uint8_t s_bl_level;   /* index into BL_LEVELS, or OBSERVORE_BRIGHT_AUTO */

#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
/* The sensor learns its own range rather than being told one.
 *
 * Fixed thresholds assume a particular sensor exposure and a particular room,
 * and the first attempt had both wrong: taken from a bare board where covering
 * it reached 1,700 counts, they left a cased board in a lit room producing
 * 1,157 to 1,298 -- the whole span below the second edge, so the two dim steps
 * could never be reached. A case over the photoresistor is enough to do that,
 * and there is no reason a person should have to know it happened.
 *
 * So the extremes seen are remembered and the steps divide whatever range the
 * board actually experiences. The span has to be wide enough to mean something
 * before it acts at all: in a room of unchanging light, min and max converge
 * and every flicker would otherwise swing the backlight. */
#define LDR_MIN_SPAN   120    /* counts, below which the light is not telling us anything */
#define LDR_HYSTERESIS 8      /* per cent of the span */

static int s_ldr_low = INT_MAX;
static int s_ldr_high = INT_MIN;

static adc_oneshot_unit_handle_t s_ldr;
static int s_ldr_channel = -1;
static int s_ldr_chosen;          /* the level the sensor last settled on */

static void ldr_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &s_ldr) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC1: the light sensor cannot be read");
        return;
    }
    if (adc_oneshot_io_to_channel(CONFIG_OBSERVORE_DISPLAY_LDR_GPIO,
                                  &(adc_unit_t){0},
                                  (adc_channel_t *)&s_ldr_channel) != ESP_OK) {
        ESP_LOGW(TAG, "gpio %d is not an ADC1 pin", CONFIG_OBSERVORE_DISPLAY_LDR_GPIO);
        s_ldr_channel = -1;
        return;
    }
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(s_ldr, (adc_channel_t)s_ldr_channel, &chan);
}

/* The level the light suggests, with the hysteresis applied against whatever
 * was chosen last. */
static int ldr_level(void)
{
    int raw = 0;
    if (s_ldr_channel < 0 ||
        adc_oneshot_read(s_ldr, (adc_channel_t)s_ldr_channel, &raw) != ESP_OK) {
        return s_ldr_chosen;
    }
#if !CONFIG_OBSERVORE_DISPLAY_LDR_DARK_IS_HIGH
    raw = 4095 - raw;
#endif
    if (raw < s_ldr_low)  s_ldr_low = raw;
    if (raw > s_ldr_high) s_ldr_high = raw;
    int span = s_ldr_high - s_ldr_low;
    if (span < LDR_MIN_SPAN) {
        /* Not enough variation to divide. Hold whatever is showing rather than
         * inventing steps out of noise. */
        return s_ldr_chosen;
    }

    int level = 0;
    int hyst = span * LDR_HYSTERESIS / 100;
    for (int i = 0; i < BL_COUNT - 1; i++) {
        /* Edges spread evenly across the range this board has actually seen.
         * Moving towards dimmer clears the edge; moving back towards brighter
         * has to clear it by the hysteresis too, or a reading sitting on a
         * boundary makes the panel pulse every time somebody passes a lamp. */
        int edge = s_ldr_low + span * (i + 1) / BL_COUNT;
        if (s_ldr_chosen > i) {
            edge -= hyst;
        }
        if (raw > edge) {
            level = i + 1;
        }
    }
    /* Said when it moves, never while it sits still. The thresholds above came
     * from two readings on one board, and the only way to know whether they
     * suit a real room is to watch what the room produces. */
    if (level != s_ldr_chosen) {
        ESP_LOGI(TAG, "ambient %d counts (range %d-%d) -> brightness step %d",
                 raw, s_ldr_low, s_ldr_high, level);
    }
    s_ldr_chosen = level;
    return level;
}
#endif

static void ui_task(void *arg);
#if CONFIG_OBSERVORE_TOUCH
static void draw_buttons(void);
#endif

static void bl_apply(void)
{
#if !CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }
#endif
    uint8_t level = s_bl_level;
#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
    if (level == OBSERVORE_BRIGHT_AUTO) {
        level = (uint8_t)ldr_level();
    }
#endif
    if (level >= BL_COUNT) {
        level = 0;
    }
#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    /* No backlight to dim: an AMOLED lights each pixel itself, and the panel
     * scales them for us. The same four steps, sent as a command.
     *
     * Over four data lines the command does not travel as itself. There is no
     * D/C pin, so the opcode goes in the address phase: a write is 0x02, then
     * the register, then a pad byte, packed into the 32-bit command word the
     * IO layer sends. Sending a bare 0x51 is silently ignored by the panel,
     * which is exactly what it did -- the initialisation sequence worked
     * because the driver wraps its own commands this way, and mine did not,
     * so the brightness button cycled four settings and changed nothing. */
    if (s_io) {
        uint8_t duty = BL_LEVELS[level];
        int cmd = (int)((0x02u << 24) | ((uint32_t)PANEL_BRIGHTNESS_CMD << 8));
        esp_lcd_panel_io_tx_param(s_io, cmd, &duty, 1);
    }
#else
    ledc_set_duty(BL_MODE, BL_CHANNEL, BL_LEVELS[level]);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
#endif
}

/* Nesting depth of backlight holds; see the header for why they exist. */
static int s_bl_held;

void observore_display_backlight_hold(void)
{
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }
    if (s_bl_held++ == 0) {
        ledc_set_duty(BL_MODE, BL_CHANNEL, 0);
        ledc_update_duty(BL_MODE, BL_CHANNEL);
        /* Let the driver stage actually settle before the measurement that
         * this call exists to protect. */
        esp_rom_delay_us(200);
    }
}

void observore_display_backlight_release(void)
{
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }
    if (s_bl_held > 0 && --s_bl_held == 0) {
        bl_apply();
    }
}

static void bl_save(void)
{
    uint8_t v = s_bl_level;
    observore_nvs_item_t item = {.key = "bright", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &v, .len = sizeof(v)};
    observore_nvs_write(&item, 1);
}

static void bl_init(void)
{
#if !CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }
#endif

    uint8_t v = 0;
    observore_nvs_item_t item = {.key = "bright", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &v, .len = sizeof(v)};
    if (observore_nvs_read(&item, 1) == ESP_OK && item.found &&
        v <= OBSERVORE_BRIGHT_AUTO) {
        s_bl_level = v;
    }
#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
    ldr_init();
    /* A board with a sensor starts by using it. The screen this device most
     * wants is the dim one, and asking a person to remember to set that every
     * time defeats the point. */
    if (!item.found) {
        s_bl_level = OBSERVORE_BRIGHT_AUTO;
    }
#endif
    if (s_bl_level == OBSERVORE_BRIGHT_AUTO && !observore_display_has_light_sensor()) {
        s_bl_level = 0;
    }

#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    /* Nothing to set up: the stored level goes straight to the panel. */
    bl_apply();
    return;
#else
    ledc_timer_config_t timer = {
        .speed_mode      = BL_MODE,
        .duty_resolution = BL_BITS,
        .timer_num       = BL_TIMER,
        .freq_hz         = 5000,   /* above hearing, below anything the eye sees */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGW(TAG, "backlight timer unavailable; leaving it full on");
        return;
    }
    ledc_channel_config_t ch = {
        .gpio_num   = CONFIG_OBSERVORE_DISPLAY_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = BL_LEVELS[s_bl_level],
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGW(TAG, "backlight channel unavailable; leaving it full on");
        return;
    }
    bl_apply();
#endif
}

void observore_display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no memory for the display lock");
        return;
    }

#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    /* Which controller is behind the glass, asked rather than assumed. The
     * pins must be borrowed for this, so it happens before the SPI bus
     * exists. */
    bool co5300 = CONFIG_OBSERVORE_DISPLAY_CO5300;
#if CONFIG_OBSERVORE_DISPLAY_PANEL_DETECT
    uint8_t id = panel_id();
    co5300 = (id != PANEL_ID_SH8601);
    ESP_LOGI(TAG, "panel answered 0x%02X: %s", id,
             co5300 ? "CO5300" : "SH8601");
#endif
    s_x_off = co5300 ? 6 : 0;

    /* Four data lines and no D/C pin: the command travels in the address
     * phase instead, which is what the QSPI flag below selects. */
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_OBSERVORE_DISPLAY_SCLK,
        .data0_io_num = CONFIG_OBSERVORE_DISPLAY_QSPI_D0,
        .data1_io_num = CONFIG_OBSERVORE_DISPLAY_QSPI_D1,
        .data2_io_num = CONFIG_OBSERVORE_DISPLAY_QSPI_D2,
        .data3_io_num = CONFIG_OBSERVORE_DISPLAY_QSPI_D3,
        .flags = SPICOMMON_BUSFLAG_QUAD,
        /* Room for several strips rather than exactly one. Sized to the strip
         * with no headroom, a transfer that needs even a byte of framing has
         * nowhere to put it, and the rows it could not send stay as they were
         * -- which reads as horizontal banding across a redrawn frame rather
         * than as an error anybody logs. */
        .max_transfer_sz = CLEAR_STRIP_BYTES * 4,
    };
#else
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_OBSERVORE_DISPLAY_MOSI,
        .miso_io_num = CONFIG_OBSERVORE_DISPLAY_MISO,
        .sclk_io_num = CONFIG_OBSERVORE_DISPLAY_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_cells[0]),
    };
#endif
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_OBSERVORE_DISPLAY_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = CONFIG_OBSERVORE_DISPLAY_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
#if CONFIG_OBSERVORE_WATCHFACE
        .on_color_trans_done = blit_done,
#endif
    };
#else
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_OBSERVORE_DISPLAY_CS,
        .dc_gpio_num = CONFIG_OBSERVORE_DISPLAY_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_OBSERVORE_DISPLAY_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
#endif
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    sh8601_vendor_config_t vendor = {
        .init_cmds = co5300 ? CO5300_INIT : SH8601_INIT,
        .init_cmds_size = co5300 ? sizeof(CO5300_INIT) / sizeof(CO5300_INIT[0])
                                 : sizeof(SH8601_INIT) / sizeof(SH8601_INIT[0]),
        .flags = { .use_qspi_interface = 1 },
    };
#endif
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_OBSERVORE_DISPLAY_RST,
        .rgb_ele_order = CONFIG_OBSERVORE_DISPLAY_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                                      : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
        .vendor_config = &vendor,
#endif
    };
    s_io = io;
    err = new_panel(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, PANEL_NAME ": %s", esp_err_to_name(err));
        return;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* The ST7789 revision of this board is widely reported to need inversion
     * and on the bench did not; the ILI9341 one is not expected to. It is a
     * build setting either way, because the failure looks like a negative and
     * is obvious the moment the screen is looked at. */
    esp_lcd_panel_invert_color(s_panel, CONFIG_OBSERVORE_DISPLAY_INVERT);
#if !CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    /* The rectangular panels are portrait parts used in landscape. The round
     * one is square and arrives the right way up. */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, CONFIG_OBSERVORE_DISPLAY_MIRROR_X,
                         CONFIG_OBSERVORE_DISPLAY_MIRROR_Y);
#endif
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Before anything draws, while the internal heap is still unfragmented
     * and the radios have not taken their share -- and before the first
     * clear, which is the first thing to need it. */
    s_strip = heap_caps_malloc(CLEAR_STRIP_BYTES, MALLOC_CAP_DMA);
    if (!s_strip) {
        ESP_LOGW(TAG, "no DMA strip; full-panel writes will be skipped");
    }
    clear_panel();


#if CONFIG_OBSERVORE_WATCHFACE
    s_blit_done = xSemaphoreCreateBinary();
#endif
    memset(s_shown, 0, sizeof(s_shown));
    s_ready = true;
    for (int r = 0; r < ROWS; r++) {
        line(r, "", C_WHITE, C_BLACK);
    }
    line(0, "  OBSERVORE", C_BLACK, C_GREY);
    line(1, "  starting", C_BLACK, C_GREY);
    /* Backlight last, so nobody watches the panel initialise. */
    bl_init();

    if (xTaskCreate(ui_task, "ui", UI_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the drawing task");
    }
#if CONFIG_OBSERVORE_DISPLAY_QSPI_AMOLED
    ESP_LOGI(TAG, "%s %dx%d, %d columns, x offset %d",
             co5300 ? "CO5300" : "SH8601", DISP_W, DISP_H, COLS, s_x_off);
#else
    ESP_LOGI(TAG, PANEL_NAME " %dx%d, %d columns", DISP_W, DISP_H, COLS);
#endif
}

static const char *ago(int64_t us, char *buf, size_t len)
{
    int64_t s = us / 1000000;
    if (s < 3600) {
        snprintf(buf, len, "%lldm", (long long)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, len, "%lldh%02lldm", (long long)(s / 3600), (long long)((s % 3600) / 60));
    } else {
        snprintf(buf, len, "%lldd%02lldh", (long long)(s / 86400), (long long)((s % 86400) / 3600));
    }
    return buf;
}

/* The watch page: what the device sees. Unchanged in substance from the
 * screen this board has had since it gained one. */
static void draw_watch(const observore_status_t *st,
                       const observore_event_t *top, size_t n,
                       int64_t now_us)
{
    char text[COLS + 1];

    /* The band: two rows in the level's colour, level word and score. */
    uint16_t band = st->level == OBSERVORE_LEVEL_ALERT   ? C_RED
                  : st->level == OBSERVORE_LEVEL_CAUTION ? C_AMBER
                                                         : C_GREEN;
    snprintf(text, sizeof(text), "  OBSERVORE  %-8s  score %u",
             observore_level_name(st->level), st->score);
    line(0, text, C_BLACK, band);
    /* The count of switched-off monitors sits in the band itself, beside the
     * verdict, because that is the only place it cannot be missed.
     *
     * A device that has been told to stop looking for something must not say
     * "clear" as though it had looked. Unlike a mute rule, a disabled monitor
     * leaves nothing in the data to find afterwards -- no suppressed count,
     * no rule to read -- so if the verdict does not carry it, nothing does. */
    /* Composed in a buffer sized for the text rather than for the grid: the
     * narrowest panel here is forty columns, and the compiler has to assume a
     * %u could be five digits. line() takes what fits. */
    char bar[80];
    if (st->monitors_off > 0) {
        snprintf(bar, sizeof(bar), "  %u device%s   %d monitor%s OFF",
                 st->device_count, st->device_count == 1 ? "" : "s",
                 st->monitors_off, st->monitors_off == 1 ? "" : "s");
    } else {
        snprintf(bar, sizeof(bar), "  %u device%s   %lu sightings",
                 st->device_count, st->device_count == 1 ? "" : "s",
                 (unsigned long)st->total_sightings);
    }
    line(1, bar, C_BLACK, band);

    int row = 2;
    /* Something the person at the device just did, for a few seconds. */
    if (s_notice[0] && now_us < s_notice_until_us) {
        line(row++, s_notice, C_BLACK, C_AMBER);
    } else {
        s_notice[0] = '\0';
    }

    /* Findings, most recent first: class, address, signal, and what it was
     * called. Same facts as a digest line, fitted to forty columns. */
    const int last_row = ROWS - BAR_LAST;
#if CONFIG_OBSERVORE_TOUCH
    for (size_t r = 0; r < ROWS; r++) {
        s_row_finding[r] = -1;
    }
#endif
    for (size_t i = 0; i < n && row < last_row; i++, row++) {
        char mac[OBSERVORE_MAC_STR_LEN];
        observore_mac_str(top[i].mac, mac);
        const char *who = top[i].detail[0] ? top[i].detail
                        : top[i].vendor    ? top[i].vendor
                        : top[i].addr_random ? "random" : "";
        /* Do not print the same word twice on one row. A Flipper broadcasts
         * "Flipper Arala75h" and we label it "Flipper Zero", so the row read
         * "Flipper Ara... Flipper Zero" and spent eight columns saying nothing.
         * Where the name opens with the label's first word, that word is
         * already covered and the rest of the name is the part that
         * identifies which one. */
        if (top[i].label[0] && who == top[i].detail) {
            const char *space = strchr(top[i].label, ' ');
            size_t first = space ? (size_t)(space - top[i].label)
                                 : strlen(top[i].label);
            if (first > 0 && strncasecmp(who, top[i].label, first) == 0 &&
                who[first] == ' ' && who[first + 1] != '\0') {
                who += first + 1;
            }
        }
        /* What we think it is, where the screen is wide enough to say so.
         *
         * A row shows the class and the device's own broadcast name, so a
         * Flipper read "hunter" and left the reader to know what that meant --
         * "Flipper Zero" lives in the label, which the glass never rendered at
         * all. Forty columns genuinely has no room; sixty does, and the space
         * was sitting empty. */
#if COLS >= 56
        /* Checked by the preprocessor, not at runtime: a runtime test leaves
         * the wide branch compiled on the narrow panel, where the compiler can
         * see it will not fit and refuses the build. Which is also what stops
         * the label from being cut to "Find My trac" -- class, address, signal
         * and name take thirty-two columns, leaving sixteen for the label on a
         * sixty-column screen, and "Find My tracker" is fifteen. */
        if (top[i].label[0]) {
            snprintf(text, sizeof(text), "%-8.8s %s %4d %-11.11s %.16s",
                     observore_class_name(top[i].cls), mac, top[i].rssi,
                     who, top[i].label);
        } else
#endif
        {
            snprintf(text, sizeof(text), "%-8.8s %s %4d %.8s",
                     observore_class_name(top[i].cls), mac, top[i].rssi, who);
        }
        uint16_t fg = top[i].cls == OBSERVORE_CLASS_FOLLOWER ? C_WHITE : C_AMBER;
#if CONFIG_OBSERVORE_TOUCH
        /* Which finding is on which row, so a tap can name the thing under the
         * finger rather than an index into a list that has since moved. */
        s_row_finding[row] = (int)i;
        if (row == s_armed_row && now_us < s_armed_until_us) {
            /* The row keeps showing what it is about, cut to whatever the
             * question leaves: forty columns on the 2.8" boards is not much. */
            char ask[COLS + 1];
            /* A fixed slice of the row, not the whole of it: the buffer is
             * the screen's width and forty columns does not stretch. */
            snprintf(ask, sizeof(ask), " ignore? tap again  %.17s", text);
            line(row, ask, C_BLACK, C_AMBER);
            continue;
        }
#endif
        line(row, text, fg, C_BLACK);
    }
    if (n == 0) {
        line(row++, "  nothing classified", C_GREY, C_BLACK);
    }
    for (; row < last_row; row++) {
        line(row, "", C_WHITE, C_BLACK);
    }

}

/* The system page: what the device is, rather than what it sees. Everything
 * here is already on the console; the point is that it is legible without
 * one, which is the whole argument for the screen. */
#if CONFIG_OBSERVORE_WATCHFACE
/* The dial.
 *
 * Drawn into a buffer and blitted once a second, which costs about four
 * hundred kilobytes a second over the bus and is why this exists only on the
 * board with the PSRAM to hold it. A pocket watch with no seconds hand would
 * be cheaper and would also look like a screenshot.
 *
 * The threat level is the colour of the hour markers and nothing else. That
 * was a deliberate choice over a banner: the page's whole purpose is that a
 * stranger glancing at it sees somebody checking the time, and a red warning
 * across the face gives away precisely what the disguise was for. Somebody
 * who knows the device reads amber markers instantly; nobody else reads
 * anything. */
static uint16_t *s_fb;
/* When the face was last drawn, and until when it should keep a seconds hand.
 *
 * A dial redrawn every second pushes four hundred kilobytes a second at the
 * panel in twenty-nine separate transfers, which tears visibly and costs
 * power all day for a hand nobody is watching. So the face is still by
 * default -- minute and hour only, redrawn when the minute changes, the way a
 * pocket watch with no subsidiary seconds behaves -- and a tap wakes a
 * seconds hand for fifteen seconds, for when somebody actually is looking. */
static int     s_face_minute = -1;
static int64_t s_face_awake_until_us;

/* How far the dial wanders from centre. Three pixels moves the ring clear of
 * its own width -- it is five thick, so a three-pixel walk leaves no pixel
 * lit by it at every step -- while costing three pixels of radius on a dial
 * 229 across, which is not a difference anybody can see. */
#define FACE_SHIFT_R  3
#define FACE_MARGIN   4
#define FACE_RING_W   5

void observore_display_wake_face(void)
{
    s_face_awake_until_us = esp_timer_get_time() + 15 * 1000000;
    s_face_minute = -1;
}

static void draw_clockface(const observore_status_t *st)
{
    if (!s_fb) {
        s_fb = heap_caps_malloc((size_t)DISP_W * DISP_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_fb) {
            ESP_LOGW(TAG, "no room for a watch face");
            return;
        }
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    /* What was last actually put on the glass. Redrawing only when the
     * minute turns is right for a still face and wrong for everything else
     * that changes what the face should look like.
     *
     * Two of those were missed. Coming back to this page within the same
     * minute drew nothing at all, so the findings page stayed on the screen
     * until the minute happened to turn -- which reads as a watch face that
     * has stopped working. And when the fifteen seconds of wakefulness
     * lapsed, the button bar and the seconds hand stayed drawn, because
     * nothing redrew to take them away: a menu that would not go away.
     *
     * The rule is the same in all cases: redraw when what should be on the
     * glass differs from what is. */
    static int  s_face_level = -1;
    static bool s_face_drew_awake;
    bool awake = esp_timer_get_time() < s_face_awake_until_us;
    if (!awake && lt.tm_min == s_face_minute && (int)st->level == s_face_level &&
        awake == s_face_drew_awake) {
        return;                       /* nothing has moved that anybody can see */
    }
    s_face_minute     = lt.tm_min;
    s_face_level      = (int)st->level;
    s_face_drew_awake = awake;

    observore_canvas_t c = {.px = s_fb, .w = DISP_W, .h = DISP_H};

    /* The dial walks, so that an AMOLED showing the same ring for a month
     * does not keep it.
     *
     * The step comes from the clock rather than from a counter, which makes
     * it continuous across a reboot -- a device restarted every morning
     * would otherwise begin every day on the same eight pixels -- and costs
     * nothing, since the face already knows what time it is.
     *
     * It is held still while the face is awake. The walk is about two pixels
     * a step and nobody would call it wrong, but a dial that twitches at the
     * minute while you are looking at it is a thing you would notice, and
     * the whole point of the page is that it looks like a watch. Burn-in
     * accrues over the hours nobody is looking, which is exactly when this
     * is free to move.
     *
     * Deriving it from the clock has one discontinuity, and it is worth
     * naming rather than discovering: when SNTP first sets the time the step
     * jumps from boot-relative to epoch-relative, so the dial moves once to
     * an unrelated position. It happens at most once a run, only while idle,
     * and the alternative -- counting minutes instead -- trades it for
     * starting every boot on the same eight pixels, which is the thing this
     * is here to avoid. */
    static unsigned s_shift_step;
    if (!awake) {
        s_shift_step = (unsigned)(now / 60);
    }
    int sdx = 0, sdy = 0;
    observore_watch_shift(s_shift_step, FACE_SHIFT_R, &sdx, &sdy);

    const int cx = DISP_W / 2 + sdx, cy = DISP_H / 2 + sdy;
    /* The margin as before, and the walk and the ring's own thickness on top:
     * the ring has to be inside the glass at every step, not just at the one
     * it happened to be drawn at on the bench. The arithmetic is in
     * observore_watch.c so a host test can hold it to that. */
    const int r  = observore_watch_dial_radius(DISP_W, DISP_H, FACE_MARGIN,
                                               FACE_RING_W, FACE_SHIFT_R);

    /* Black is genuinely off on this panel, so an unlit dial costs nothing
     * to show and little to leave on. */
    observore_watch_fill(&c, px(C_BLACK));
    observore_watch_ring(&c, cx, cy, r, FACE_RING_W, px(C_SILVER));

    uint16_t mark = st->level == OBSERVORE_LEVEL_ALERT   ? px(C_RED)
                  : st->level == OBSERVORE_LEVEL_CAUTION ? px(C_AMBER)
                                                         : px(C_GREY);

    /* Hour markers: longer at the quarters, and the colour carries the
     * verdict. Minute ticks are deliberately absent -- at this radius they
     * turn into a grey band. */
    for (int h = 0; h < 12; h++) {
        int x0, y0, x1, y1;
        int len = (h % 3 == 0) ? 26 : 14;
        observore_watch_hand_end(cx, cy, r - 8, h, 12, &x0, &y0);
        observore_watch_hand_end(cx, cy, r - 8 - len, h, 12, &x1, &y1);
        observore_watch_line(&c, x0, y0, x1, y1, (h % 3 == 0) ? 7 : 3, mark);
    }

    /* The date, where a pocket watch keeps it. */
    char date[16];
    strftime(date, sizeof(date), "%a %d %b", &lt);
    observore_watch_text(&c, cx, cy + r / 2 - 8, date, 2, px(C_GREY));

    /* Hands. The hour hand moves with the minutes, as a real one does, which
     * is 720 positions round the dial rather than twelve. */
    int hx, hy, mx, my, sx, sy;
    observore_watch_hand_end(cx, cy, r - 130, lt.tm_hour % 12 * 60 + lt.tm_min,
                             720, &hx, &hy);
    observore_watch_hand_end(cx, cy, r - 70, lt.tm_min * 60 + lt.tm_sec,
                             3600, &mx, &my);
    observore_watch_hand_end(cx, cy, r - 40, lt.tm_sec, 60, &sx, &sy);

    observore_watch_hand(&c, cx, cy, hx, hy, 26, 13, 5, px(C_WHITE));
    observore_watch_hand(&c, cx, cy, mx, my, 30, 9,  3, px(C_WHITE));
    if (awake) {
        observore_watch_hand(&c, cx, cy, sx, sy, 34, 3, 3, mark);
    }
    observore_watch_disc(&c, cx, cy, 10, px(C_WHITE));
    observore_watch_disc(&c, cx, cy, 5,  px(C_BLACK));

    /* No time yet is said rather than drawn as midnight, which is what an
     * unset clock would otherwise claim with total confidence. */
    if (!observore_clock_valid()) {
        observore_watch_text(&c, cx, cy - r / 2, "not set", 2, px(C_AMBER));
    }

    /* Out in strips through a small buffer in internal memory.
     *
     * The frame itself lives in PSRAM, which the SPI driver cannot DMA from:
     * asked to send four hundred kilobytes it tries to allocate a private
     * bounce buffer of the whole transfer and fails, once per frame, silently
     * apart from two error lines. So the copy is explicit and bounded, and
     * the strip is the same size the panel clear already uses. */
    uint16_t *strip = s_strip;
    if (!strip) {
        ESP_LOGW(TAG, "no DMA buffer for the watch face");
        return;
    }
    const int rows = CLEAR_STRIP_BYTES / (DISP_W * 2);
    /* Anything posted by the text path before this is stale. */
    while (xSemaphoreTake(s_blit_done, 0) == pdTRUE) { }
    for (int y = 0; y < DISP_H; y += rows) {
        int h = (y + rows <= DISP_H) ? rows : DISP_H - y;
        memcpy(strip, &s_fb[(size_t)y * DISP_W], (size_t)h * DISP_W * 2);
        esp_lcd_panel_draw_bitmap(s_panel, X_OFF, y, X_OFF + DISP_W, y + h,
                                  strip);
        /* The buffer is reused on the next pass, so the panel has to be
         * finished with it first. */
        xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(100));
    }

    /* Awake, the face shows its buttons; idle, it does not.
     *
     * They work either way -- the taps were always live -- but an invisible
     * control is not a control, and "how do I get back to the detector" is
     * not a question a person should have to ask. Hiding them while idle is
     * the point of the page: a bar reading page / baseline / light across a
     * watch face gives the game away as surely as a warning banner would. */
    if (awake) {
        draw_buttons();
    }
    /* The text grid knows nothing about what just happened to the panel, so
     * every row is marked stale and will be redrawn when a page returns. */
    memset(s_shown, 0, sizeof(s_shown));
}
#endif

#if CONFIG_OBSERVORE_TOUCH_XPT2046
/* Two targets, in opposite corners, inset far enough to be pressable.
 *
 * The bezel on these boards overlaps the glass, and a press right at the
 * edge often does not register at all -- which is how the 3.5" panel's first
 * calibration came out wrong and squashed the bottom three rows together.
 * An inset of two characters is comfortably inside the usable area, and the
 * arithmetic below accounts for it rather than pretending the press was at
 * the very corner. */
#define CAL_INSET_COLS 2
#define CAL_INSET_ROWS 2

static void draw_calibrate(void)
{
    char text[COLS + 1];
    for (int r = 0; r < ROWS; r++) {
        line(r, "", C_WHITE, C_BLACK);
    }
    line(1, "  CALIBRATE TOUCH", C_BLACK, C_GREY);
    snprintf(text, sizeof(text), "  press the marked corner, %s of 2",
             s_cal_step == 0 ? "1" : "2");
    line(3, text, C_WHITE, C_BLACK);
    line(5, "  hold briefly, then let go", C_GREY, C_BLACK);
    line(ROWS - 2, "  tap anywhere else to give up", C_GREY, C_BLACK);

    /* The target itself: a cross of characters, which the text grid can draw
     * and which is unambiguous about where to press. */
    int col = s_cal_step == 0 ? CAL_INSET_COLS : COLS - 1 - CAL_INSET_COLS;
    int row = s_cal_step == 0 ? CAL_INSET_ROWS : ROWS - 1 - CAL_INSET_ROWS;
    char marker[COLS + 1];
    memset(marker, ' ', COLS);
    marker[COLS] = '\0';
    marker[col] = '+';
    line(row, marker, C_AMBER, C_BLACK);
    if (col > 0) { marker[col] = ' '; }
}

/* Called from the drawing loop while calibrating: the raw reading is wanted
 * here rather than a mapped one, since the mapping is what is being fixed. */
static void calibrate_poll(void)
{
    int rx = 0, ry = 0;
    if (!observore_touch_raw(&rx, &ry)) {
        return;
    }
    if (esp_timer_get_time() < s_cal_ready_us) {
        return;              /* still the press that started this step */
    }
    s_cal_raw_x[s_cal_step] = rx;
    s_cal_raw_y[s_cal_step] = ry;
    s_cal_step++;
    s_cal_ready_us = esp_timer_get_time() + 1200 * 1000;
    if (s_cal_step < 2) {
        s_dirty = true;
        return;
    }

    /* Where the two targets actually were, in pixels: the centre of the grid
     * cell each cross was drawn in.
     *
     * Pixels rather than cells, because the arithmetic pairs a raw channel
     * with a screen axis and the two axes of this grid are not the same shape
     * -- a cell is eight pixels wide and sixteen tall. Working in cells and
     * then dividing by a count of cells happens to cancel on one axis and
     * not the other, which is the sort of thing that looks right in the
     * source and is wrong on the glass. */
    const int tx0 = INSET_X + CAL_INSET_COLS * OBSERVORE_FONT_W + OBSERVORE_FONT_W / 2;
    const int ty0 = INSET_Y + CAL_INSET_ROWS * OBSERVORE_FONT_H + OBSERVORE_FONT_H / 2;
    const int tx1 = INSET_X + (COLS - 1 - CAL_INSET_COLS) * OBSERVORE_FONT_W
                    + OBSERVORE_FONT_W / 2;
    const int ty1 = INSET_Y + (ROWS - 1 - CAL_INSET_ROWS) * OBSERVORE_FONT_H
                    + OBSERVORE_FONT_H / 2;

    observore_touchcal_t cal;
    bool solved = observore_touchcal_solve(
        s_cal_raw_x[0], s_cal_raw_y[0], s_cal_raw_x[1], s_cal_raw_y[1],
        tx0, ty0, tx1, ty1, DISP_W, DISP_H,
#if CONFIG_OBSERVORE_TOUCH_SWAP_XY
        true,
#else
        false,
#endif
        &cal);

    s_cal_step = -1;
    if (!solved) {
        /* The presses could not describe a sheet -- the same spot twice, or a
         * channel answering with a stuck value. Keeping the old bounds beats
         * installing a mapping already known to be wrong. */
        snprintf(s_notice, sizeof(s_notice), " calibration not usable -- unchanged");
        s_notice_until_us = esp_timer_get_time() + 8 * 1000000;
        s_dirty = true;
        return;
    }

    observore_touch_set_bounds(cal.lo_x, cal.hi_x, cal.lo_y, cal.hi_y);
    snprintf(s_notice, sizeof(s_notice), " calibrated: x %d-%d y %d-%d",
             cal.lo_x, cal.hi_x, cal.lo_y, cal.hi_y);
    s_notice_until_us = esp_timer_get_time() + 8 * 1000000;
    s_dirty = true;
}
#endif

static void draw_system(const observore_status_t *st, int64_t now_us)
{
    char text[COLS + 1], up[16];
    const esp_app_desc_t *app = esp_app_get_description();

    /* A running row rather than numbered lines. The page grew a line for the
     * backlight and every row below it had to be renumbered by hand, which on
     * the shorter panel silently pushed the run history off the bottom. */
    int r = 0;
    line(r++, "  OBSERVORE  system", C_BLACK, C_GREY);
    snprintf(text, sizeof(text), " version  %.28s", app ? app->version : "?");
    line(r++, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " board    %.28s", CONFIG_OBSERVORE_BOARD);
    line(r++, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " up       %.28s", ago(now_us, up, sizeof(up)));
    line(r++, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " seen     %u device%s, %lu sightings",
             st->device_count, st->device_count == 1 ? "" : "s",
             (unsigned long)st->total_sightings);
    line(r++, text, C_WHITE, C_BLACK);

#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
    /* Only where the light sensor makes the setting ambiguous. Cycling a
     * button with no indication of where you are in the cycle is how a person
     * ends up asking whether auto is even switched on -- and on a board
     * without a sensor the brightness speaks for itself. */
    {
        static const char *const names[] = {"full", "bright", "dim", "dimmest"};
        int set = observore_display_brightness();
        int now = observore_display_brightness_effective();
        if (set == OBSERVORE_BRIGHT_AUTO) {
            snprintf(text, sizeof(text), " light    auto (%s)",
                     now >= 0 && now < 4 ? names[now] : "?");
        } else {
            snprintf(text, sizeof(text), " light    %s",
                     set >= 0 && set < 4 ? names[set] : "?");
        }
        line(r++, text, C_WHITE, C_BLACK);
    }
#endif

#if CONFIG_OBSERVORE_BATTERY
    if (observore_battery_available()) {
        int mv = observore_battery_mv();
        int pct = observore_battery_pct_from_mv(mv);
        if (mv > 0) {
            /* Voltage as well as percent, because the percent comes off a
             * coarse curve and the volts are what was measured. On USB this
             * reads the charger rather than a discharging cell, which is why
             * it does not claim to know which. */
            snprintf(text, sizeof(text), " battery  %d.%02d V, about %d%%",
                     mv / 1000, (mv % 1000) / 10, pct);
        } else {
            snprintf(text, sizeof(text), " battery  no reading");
        }
        line(r++, text, C_WHITE, C_BLACK);
    }
#endif

#if CONFIG_OBSERVORE_MOTION
    /* Only where there is a sensor to report. Three facts in one row: whether
     * it is being carried now, how many journeys have counted, and what the
     * last one measured -- which is the number that says whether a trip was
     * judged to have gone anywhere. */
    if (observore_motion_available()) {
        int ov = observore_motion_last_overlap_pct();
        uint32_t trips = observore_motion_journeys();
        char trip_s[8], tail[20];
        snprintf(trip_s, sizeof(trip_s), "%lu", (unsigned long)trips);
        int fade = observore_motion_last_faded_db();
        if (ov < 0) {
            snprintf(tail, sizeof(tail), "no trip");
        } else if (fade != INT_MIN) {
            /* Both halves of the question: how many of the old access points
             * are still audible, and how much fainter they have become. */
            snprintf(tail, sizeof(tail), "%d%% %ddB", ov > 100 ? 100 : ov, fade);
        } else {
            /* How much of the old place came back with it -- the number that
             * says whether a trip counted, and the one worth reading off the
             * glass after one. */
            snprintf(tail, sizeof(tail), "%d%% same", ov > 100 ? 100 : ov);
        }
        /* Every field bounded, because the row has to fit a 41-column grid
         * and the compiler is right to insist rather than trust me. */
        snprintf(text, sizeof(text), " motion  %.7s %.4s trip%.1s %.9s",
                 observore_motion_moving() ? "carried" : "still",
                 trip_s, trips == 1 ? "" : "s", tail);
        line(r++, text, C_WHITE, C_BLACK);
    }
#endif

    snprintf(text, sizeof(text), " heap     %u free, %u least",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    line(r++, text, C_WHITE, C_BLACK);

    /* The address, which is the one fact on this page a person has to take
     * somewhere else -- and on a board like this there is nowhere else to read
     * it. The password is a different matter and is still never drawn. */
    const char *ip = observore_wifi_uplink_ip();
    snprintf(text, sizeof(text), " console  %.28s",
             ip && ip[0] ? ip : "only during an uplink window");
    line(r++, text, C_WHITE, C_BLACK);

    const char *latest = observore_update_latest_version();
    if (observore_update_available()) {
        snprintf(text, sizeof(text), " update   %.12s is available", latest);
        line(r++, text, C_BLACK, C_AMBER);
    } else if (observore_update_check_pending()) {
        line(r++, " update   checking...", C_GREY, C_BLACK);
    } else if (latest && latest[0]) {
        snprintf(text, sizeof(text), " update   up to date (%.10s)", latest);
        line(r++, text, C_GREY, C_BLACK);
    } else {
        line(r++, " update   not checked yet", C_GREY, C_BLACK);
    }

    line(r++, "", C_WHITE, C_BLACK);
    line(r++, " previous runs", C_GREY, C_BLACK);

    observore_run_t runs[4];
    size_t nr = observore_runs_list(runs, 4);
    int row = r;
    for (size_t i = 0; i < nr && row < ROWS - BAR_LAST; i++, row++) {
        char dur[16];
        snprintf(dur, sizeof(dur), "%lluh%02llum",
                 (unsigned long long)(runs[i].up_s / 3600),
                 (unsigned long long)((runs[i].up_s % 3600) / 60));
        snprintf(text, sizeof(text), "   %-8.8s ended by %.18s", dur,
                 observore_reset_reason_name((esp_reset_reason_t)runs[i].end));
        line(row, text, C_WHITE, C_BLACK);
    }
    if (nr == 0 && row < ROWS - BAR_LAST) {
        line(row++, "   none recorded yet", C_GREY, C_BLACK);
    }
#if CONFIG_OBSERVORE_TOUCH
    for (; row < ACTION_TOP; row++) {
        line(row, "", C_WHITE, C_BLACK);
    }
    /* Two actions, half the width each. Install only offers itself when there
     * is something to install, and says so while it waits for the second tap:
     * it stops the detector for minutes and then reboots it. */
    char left[COLS], mid[COLS], right[COLS];
    /* Three actions across a row that is thirteen columns wide on the 2.8"
     * board and twenty on the 3.5". Labels that fit rather than labels that
     * truncate: "check update" with the s cut off looks like a typo, and a
     * clipped confirmation looks like a fault. */
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    const int cols_each = COLS / 4;
#else
    const int cols_each = COLS / 3;
#endif
    const int third = cols_each;
    const bool roomy = third >= 18;
    snprintf(left, sizeof(left), roomy ? " check for updates" : " check ver");
    if (!observore_update_available()) {
        snprintf(mid, sizeof(mid), "%s", "");
    } else if (s_install_armed && esp_timer_get_time() < s_install_armed_until_us) {
        snprintf(mid, sizeof(mid), roomy ? " install? tap again" : " install? y");
    } else if (roomy) {
        snprintf(mid, sizeof(mid), " install %.9s",
                 observore_update_latest_version());
    } else {
        snprintf(mid, sizeof(mid), " install");
    }
    /* Third action: the way out of a baseline, which until now existed only
     * in a console. The count is on the label because "clear ignores" with
     * nothing to clear should look different from the same words hiding
     * fifty-one rules. */
    size_t muted = observore_mute_count();
    if (muted == 0) {
        snprintf(right, sizeof(right), " no ignores");
    } else if (esp_timer_get_time() < s_clear_armed_until_us) {
        snprintf(right, sizeof(right), roomy ? " clear %u? tap again"
                                             : " clear %u? y", (unsigned)muted);
    } else {
        snprintf(right, sizeof(right), roomy ? " clear %u ignores"
                                             : " clear %u", (unsigned)muted);
    }
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    /* A fourth action where the panel is resistive, because those are the
     * ones whose sheet differs from the bench unit the bounds were measured
     * on. A capacitive controller reports pixels and has nothing to teach. */
    char cal[COLS];
    snprintf(cal, sizeof(cal), observore_touch_calibrated() ? " recalibrate"
                                                            : " calibrate");
    snprintf(text, sizeof(text), "%-*.*s%-*.*s%-*.*s%-*.*s",
             third, third, left, third, third, mid, third, third, right,
             COLS - 3 * third, COLS - 3 * third, cal);
#else
    snprintf(text, sizeof(text), "%-*.*s%-*.*s%-*.*s",
             third, third, left, third, third, mid,
             COLS - 2 * third, COLS - 2 * third, right);
#endif
    for (int r = ACTION_TOP; r < ACTION_ROW; r++) {
        line(r, "", C_BLACK, C_GREY);
    }
    line(ACTION_ROW, text, C_BLACK, C_GREY);
    row = ACTION_ROW + 1;
#endif
    for (; row < ROWS - BAR_LAST; row++) {
        line(row, "", C_WHITE, C_BLACK);
    }
}

/* The button bar, on the bottom row. Three targets across forty columns: a
 * person with a fingertip and a resistive panel needs them wide. */
#define BUTTONS 3
static const char *BUTTON_TEXT[BUTTONS] = {"  page", "  baseline", "  light"};

static int button_at(int x, int y)
{
    if (y < (ROWS - BAR_TOUCH_ROWS) * OBSERVORE_FONT_H) {
        return -1;
    }
    int col = x / OBSERVORE_FONT_W;
    if (col < COLS / 3)     return 0;
    if (col < 2 * COLS / 3) return 1;
    return 2;
}

static void draw_buttons(void)
{
    char text[COLS + 2];
    int w = COLS / BUTTONS;
    int pos = 0;
    for (int b = 0; b < BUTTONS; b++) {
        int width = (b == BUTTONS - 1) ? COLS - pos : w;
        pos += snprintf(text + pos, sizeof(text) - pos, "%-*.*s", width, width,
                        BUTTON_TEXT[b]);
    }
    /* A pressed button is drawn inverted for a moment: on a resistive panel
     * with no click and no haptics, that flash is the only way to know the
     * glass heard you. The label sits on the lower of the two rows, so the
     * upper one reads as part of the same target rather than as a gap. */
    for (int r = ROWS - BAR_ROWS; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            int b = button_at(col * OBSERVORE_FONT_W, (ROWS - 1) * OBSERVORE_FONT_H);
            bool hot = (b == s_pressed);
            char ch = (r == ROWS - 1) ? text[col] : ' ';
            draw_glyph(col, r, ch, hot ? C_BLACK : C_GREY,
                       hot ? C_AMBER : C_DARK);
        }
        s_shown[r][0] = '\0';
    }
}


#if CONFIG_OBSERVORE_TOUCH
/* The network page. Three states, drawn above the button bar. */
static void draw_wifi(void)
{
    char text[COLS + 1];
    char ssid[OBSERVORE_SSID_LEN] = {0};
    observore_netcfg_ssid(ssid, sizeof(ssid));

    line(0, "  OBSERVORE  network", C_BLACK, C_GREY);
    snprintf(text, sizeof(text), " joined   %.28s", ssid[0] ? ssid : "nothing yet");
    line(1, text, C_WHITE, C_BLACK);

    if (s_wifi_step == WIFI_SAVED) {
        line(2, "", C_WHITE, C_BLACK);
        line(3, s_wifi_msg, C_BLACK, C_AMBER);
        line(4, " it joins at the next uplink window.", C_GREY, C_BLACK);
        for (int r = 5; r < ROWS - BAR_LAST; r++) {
            line(r, "", C_WHITE, C_BLACK);
        }
        return;
    }

    if (s_wifi_step == WIFI_LIST) {
        line(2, " nearby, from the last patrol scan:", C_GREY, C_BLACK);
        int row = 3;
        for (size_t i = 0; i < s_ap_count && row < ROWS - BAR_LAST; i++, row++) {
            snprintf(text, sizeof(text), " %-24.24s %4d %s", s_aps[i].ssid,
                     s_aps[i].rssi, s_aps[i].secure ? "lock" : "open");
            line(row, text, C_WHITE, C_BLACK);
        }
        if (s_ap_count == 0) {
            line(row++, " none seen yet -- wait for a patrol scan", C_GREY, C_BLACK);
        }
        for (; row < ROWS - BAR_LAST; row++) {
            line(row, "", C_WHITE, C_BLACK);
        }
        return;
    }

    /* Typing. The field shows dots unless the reveal is on. */
    snprintf(text, sizeof(text), " %.20s", s_ap_chosen >= 0 ? s_aps[s_ap_chosen].ssid : "?");
    line(1, text, C_WHITE, C_BLACK);

    char shown[COLS + 1];
    bool reveal = s_reveal && esp_timer_get_time() < s_reveal_until_us;
    if (reveal) {
        snprintf(shown, sizeof(shown), "%.29s", s_pass);
    } else {
        size_t n = s_pass_len < 29 ? s_pass_len : 29;
        memset(shown, '*', n);
        shown[n] = '\0';
    }
    snprintf(text, sizeof(text), " pass %-29.29s", shown);
    line(2, text, C_BLACK, reveal ? C_AMBER : C_GREEN);

    /* The keyboard: each key three columns wide and two rows tall, which is
     * about seven millimetres -- the smallest a fingertip finds reliably. */
    for (int kr = 0; kr < KEY_ROWS; kr++) {
        const char *row_keys = KEYS[s_shift ? 1 : 0][kr];
        char top[COLS + 1], bot[COLS + 1];
        int pos = 0;
        for (int kc = 0; kc < KEY_COLS; kc++) {
            char ch = row_keys[kc] ? row_keys[kc] : ' ';
            /* The glyph in the middle of its key, whatever the key's width. */
            pos += snprintf(bot + pos, sizeof(bot) - pos, "%*c%*s",
                            KEY_W / 2 + 1, ch, KEY_W - KEY_W / 2 - 1, "");
        }
        bot[pos] = '\0';
        memset(top, ' ', (size_t)pos); top[pos] = '\0';
        line(KB_TOP + kr * KB_HEIGHT,     top, C_WHITE, C_DARK);
        line(KB_TOP + kr * KB_HEIGHT + 1, bot, C_WHITE, C_DARK);
    }

    /* Four actions across the width, ten columns each. */
    const int aw = COLS / 4;
    snprintf(text, sizeof(text), "%-*.*s%-*.*s%-*.*s%-*.*s",
             aw, aw, s_shift ? "   abc" : "   ABC#",
             aw, aw, reveal ? "   hide" : "   show",
             aw, aw, "   del", COLS - 3 * aw, COLS - 3 * aw, "   join");
    for (int r = ACTION_TOP; r < ACTION_ROW; r++) {
        line(r, "", C_BLACK, C_GREY);      /* part of the same target */
    }
    line(ACTION_ROW, text, C_BLACK, C_GREY);
    for (int r = ACTION_ROW + 1; r < ROWS - BAR_LAST; r++) {
        line(r, "", C_WHITE, C_BLACK);     /* the margin, and the bar's rows */
    }
}

/* A tap on the network page, above the button bar. */
static void wifi_tap(int x, int y)
{
    int row = y / OBSERVORE_FONT_H;

    if (s_wifi_step == WIFI_SAVED) {
        s_wifi_step = WIFI_LIST;
        return;
    }

    if (s_wifi_step == WIFI_LIST) {
        size_t i = (size_t)(row - 3);
        if (row >= 3 && i < s_ap_count) {
            s_ap_chosen = (int)i;
            s_pass_len  = 0;
            s_pass[0]   = '\0';
            s_shift     = false;
            s_reveal    = false;
            s_wifi_step = s_aps[i].secure ? WIFI_TYPING : WIFI_SAVED;
            if (!s_aps[i].secure) {
                /* An open network needs no password and no typing. */
                if (observore_netcfg_set(s_aps[i].ssid, "") == ESP_OK) {
                    snprintf(s_wifi_msg, sizeof(s_wifi_msg), " saved %.30s",
                             s_aps[i].ssid);
                } else {
                    snprintf(s_wifi_msg, sizeof(s_wifi_msg), " could not save that network");
                }
            }
        }
        return;
    }

    /* Typing. */
    if (row >= KB_TOP && row < KB_TOP + KEY_ROWS * KB_HEIGHT) {
        int kr = (row - KB_TOP) / KB_HEIGHT;
        int kc = (x / OBSERVORE_FONT_W) / KEY_W;
        if (kr < KEY_ROWS && kc < KEY_COLS) {
            char ch = KEYS[s_shift ? 1 : 0][kr][kc];
            if (ch && ch != ' ' && s_pass_len + 1 < sizeof(s_pass)) {
                s_pass[s_pass_len++] = ch;
                s_pass[s_pass_len]   = '\0';
            }
        }
        return;
    }
    if (row >= ACTION_TOP && row <= ACTION_ROW) {
        int which = (x / OBSERVORE_FONT_W) / (COLS / 4);
        if (which > 3) which = 3;
        if (which == 0) {
            s_shift = !s_shift;
        } else if (which == 1) {
            s_reveal = !s_reveal;
            /* Times out on its own: a password left legible on a screen in a
             * room is the thing this page is careful about. */
            s_reveal_until_us = esp_timer_get_time() + 15 * 1000000;
        } else if (which == 2) {
            if (s_pass_len) {
                s_pass[--s_pass_len] = '\0';
            }
        } else {
            const char *why = NULL;
            const char *ssid = s_ap_chosen >= 0 ? s_aps[s_ap_chosen].ssid : "";
            if (!observore_netcfg_valid(ssid, s_pass, &why)) {
                snprintf(s_wifi_msg, sizeof(s_wifi_msg), " %.38s", why ? why : "not valid");
            } else if (observore_netcfg_set(ssid, s_pass) == ESP_OK) {
                snprintf(s_wifi_msg, sizeof(s_wifi_msg), " saved %.30s", ssid);
            } else {
                snprintf(s_wifi_msg, sizeof(s_wifi_msg), " could not save that network");
            }
            /* However it went, the password does not stay in memory. */
            memset(s_pass, 0, sizeof(s_pass));
            s_pass_len  = 0;
            s_reveal    = false;
            s_wifi_step = WIFI_SAVED;
        }
    }
}
#endif

static void draw_current(void)
{
    if (!s_have_snap) {
        return;
    }
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    if (s_cal_step >= 0) {
        draw_calibrate();
        return;
    }
#endif
#if CONFIG_OBSERVORE_WATCHFACE
    /* Leaving the dial means clearing the whole panel, not just the rows.
     *
     * The text grid is the square inside the circle, so redrawing every row
     * of it cannot touch the four crescents outside -- and that is exactly
     * where the hour markers and the rim are drawn. Without this the
     * findings page came back with a ring of amber ticks still around it,
     * which is both untidy and, on a page meant to be read at a glance,
     * actively misleading. */
    static int s_drew_face;
    if (s_drew_face && s_page != PAGE_CLOCK) {
        s_drew_face = 0;
        clear_panel();
        memset(s_shown, 0, sizeof(s_shown));
    } else if (!s_drew_face && s_page == PAGE_CLOCK) {
        /* Arriving at the face: whatever is on the glass belongs to another
         * page, so the next draw must happen whether or not the minute has
         * turned. */
        s_face_minute = -1;
    }
    s_drew_face = (s_page == PAGE_CLOCK);
#endif
#if CONFIG_OBSERVORE_TOUCH
    if (s_page == PAGE_WIFI) {
        draw_wifi();
    } else
#endif
#if CONFIG_OBSERVORE_WATCHFACE
    if (s_page == PAGE_CLOCK) {
        draw_clockface(&s_snap_st);
        return;
    }
#endif
    if (s_page == PAGE_SYSTEM) {
        draw_system(&s_snap_st, s_snap_now_us);
    } else {
        draw_watch(&s_snap_st, s_snap_top, s_snap_n, s_snap_now_us);
    }
#if CONFIG_OBSERVORE_TOUCH
    draw_buttons();
#else
    {
        char text[COLS + 1], up[16];
        const esp_app_desc_t *app = esp_app_get_description();
        /* Truncation of the version is fine and deliberate: "v0.7.0-1-gb59a6d4"
         * cut short still identifies the build, and the row has forty columns. */
        snprintf(text, sizeof(text), " up %.10s   %.20s",
                 ago(s_snap_now_us, up, sizeof(up)), app ? app->version : "");
        line(ROWS - 1, text, C_GREY, C_BLACK);
    }
#endif
}

void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us)
{
    if (!s_ready || !st) {
        return;
    }
    if (n > SNAP_MAX) {
        n = SNAP_MAX;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap_st = *st;
    memcpy(s_snap_top, top, n * sizeof(s_snap_top[0]));
    s_snap_n = n;
    s_snap_now_us = now_us;
    s_have_snap = true;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

#if CONFIG_OBSERVORE_TOUCH

/* A tap on the watch page: the row under the finger is a finding, and two taps
 * ignore it.
 *
 * The rule is the one the console writes for a single device -- its name if it
 * broadcasts one, otherwise its address. Not its advert fingerprint: that
 * identifies a kind of device rather than an individual, and muting your own
 * tracker that way would silence a stranger's. (A baseline is allowed that
 * trade for a follower on a rotating address, because a baseline is a
 * statement about a whole room. One tap on one row is not.) */
static void watch_tap(int y)
{
    int row = y / OBSERVORE_FONT_H;
    if (row < 0 || row >= ROWS || s_row_finding[row] < 0) {
        return;
    }
    size_t i = (size_t)s_row_finding[row];
    if (i >= s_snap_n) {
        return;
    }

    if (s_armed_row != row || esp_timer_get_time() >= s_armed_until_us) {
        s_armed_row      = row;
        s_armed_until_us = esp_timer_get_time() + ARM_TIMEOUT_US;
        return;
    }
    s_armed_row = -1;

    const observore_event_t *e = &s_snap_top[i];
    observore_mute_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    /* A name rule matches as a substring, so for the classes this device
     * exists to find it is silenced by address and nothing else: dismissing
     * the body camera in front of you must not also dismiss every other one
     * of that model you ever walk past. */
    if (e->detail[0] != '\0' &&
        !observore_mute_class_needs_address_rule(e->cls)) {
        rule.kind = OBSERVORE_MUTE_NAME;
        snprintf(rule.ssid, sizeof(rule.ssid), "%s", e->detail);
    } else {
        rule.kind = OBSERVORE_MUTE_MAC;
        memcpy(rule.mac, e->mac, OBSERVORE_MAC_LEN);
    }

    char what[OBSERVORE_MAC_STR_LEN > 21 ? OBSERVORE_MAC_STR_LEN : 21];
    if (rule.kind == OBSERVORE_MUTE_NAME) {
        snprintf(what, sizeof(what), "%.20s", rule.ssid);
    } else {
        observore_mac_str(e->mac, what);
    }
    if (observore_mute_add(&rule, NULL) == ESP_OK) {
        /* And take it off the screen now rather than when it ages out half an
         * hour from now, which reads as the tap having done nothing. */
        observore_track_forget_muted();
        snprintf(s_notice, sizeof(s_notice), " ignoring %.20s", what);
    } else {
        snprintf(s_notice, sizeof(s_notice), " could not ignore %.20s", what);
    }
    s_notice_until_us = esp_timer_get_time() + 6 * 1000000;
}

/* A tap on the system page's action strip: ask for a version check, or install
 * what a check found. Installing stops detection for minutes and reboots, so
 * it asks twice, like the same button in the console. */
static void system_tap(int x, int y)
{
    int row = y / OBSERVORE_FONT_H;
    if (row < ACTION_TOP || row > ACTION_ROW) {
        return;
    }
    int col = x / OBSERVORE_FONT_W;
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    const int third = COLS / 4;
    const int last  = 3;
#else
    const int third = COLS / 3;
    const int last  = 2;
#endif
    int which = col / third;
    if (which > last) {
        which = last;
    }
#if CONFIG_OBSERVORE_TOUCH_XPT2046
    if (which == 3) {
        /* Measuring this panel's own sheet, which beats the bench unit's. */
        s_cal_step     = 0;
        s_cal_ready_us = esp_timer_get_time() + 1200 * 1000;
        s_install_armed = false;
        s_clear_armed_until_us = 0;
        s_dirty = true;
        return;
    }
#endif
    if (which == 0) {
        observore_update_check_now();
        snprintf(s_notice, sizeof(s_notice), " asking for a version check");
        s_notice_until_us = esp_timer_get_time() + 6 * 1000000;
        s_install_armed = false;
        s_clear_armed_until_us = 0;
        return;
    }
    if (which == 2) {
        /* Clearing every ignore rule. Asked twice, like the baseline that
         * usually created them: undoing a mistake should not be a tap away
         * from making a different one. */
        if (observore_mute_count() == 0) {
            return;
        }
        s_install_armed = false;
        if (esp_timer_get_time() >= s_clear_armed_until_us) {
            s_clear_armed_until_us = esp_timer_get_time() + ARM_TIMEOUT_US;
            return;
        }
        s_clear_armed_until_us = 0;
        size_t had = observore_mute_count();
        if (observore_mute_clear() == ESP_OK) {
            snprintf(s_notice, sizeof(s_notice), " cleared %u ignore rules",
                     (unsigned)had);
        } else {
            snprintf(s_notice, sizeof(s_notice), " could not clear them");
        }
        s_notice_until_us = esp_timer_get_time() + 8 * 1000000;
        return;
    }
    s_clear_armed_until_us = 0;
    if (!observore_update_available()) {
        return;
    }
    if (!s_install_armed || esp_timer_get_time() >= s_install_armed_until_us) {
        s_install_armed          = true;
        s_install_armed_until_us = esp_timer_get_time() + ARM_TIMEOUT_US;
        return;
    }
    s_install_armed = false;
    esp_err_t err = observore_update_install();
    snprintf(s_notice, sizeof(s_notice), err == ESP_OK
             ? " installing -- it restarts when done"
             : " could not start the update");
    s_notice_until_us = esp_timer_get_time() + 10 * 1000000;
}

/* One tap, routed by page. */
static void handle_tap(int x, int y)
{
#if CONFIG_OBSERVORE_WATCHFACE
    /* On the face, a tap that is not a button wakes the seconds hand: the
     * gesture somebody makes when they actually want to read a watch. */
    if (s_page == PAGE_CLOCK && button_at(x, y) < 0) {
        observore_display_wake_face();
        s_dirty = true;
        return;
    }
#endif
    int b = button_at(x, y);
    if (b < 0) {
        /* Above the bar, each page decides for itself. */
        if (s_page == PAGE_WIFI) {
            wifi_tap(x, y);
        } else if (s_page == PAGE_SYSTEM) {
            system_tap(x, y);
        } else {
            watch_tap(y);
        }
        s_dirty = true;
        return;
    }
    s_pressed = b;
    s_pressed_until_us = esp_timer_get_time() + 200 * 1000;

    switch (b) {
        case 0:
            s_page = (s_page + 1) % PAGE_COUNT;
            s_armed_row     = -1;
            s_install_armed = false;
            if (s_page == PAGE_WIFI) {
                /* Whatever the last patrol scan saw. Nothing is started here:
                 * the chip has one radio and a scan on demand would fight the
                 * sweep for it. */
                s_ap_count  = observore_wifi_last_scan(s_aps, ARRAY_SIZE(s_aps));
                s_wifi_step = WIFI_LIST;
                s_ap_chosen = -1;
                memset(s_pass, 0, sizeof(s_pass));
                s_pass_len  = 0;
            }
            break;
        case 1:
            /* The main loop owns the memory a baseline needs, so this only
             * asks. It says so on the screen, and says again when it is done. */
            if (esp_timer_get_time() >= s_baseline_armed_until_us) {
                s_baseline_armed_until_us = esp_timer_get_time() + ARM_TIMEOUT_US;
                snprintf(s_notice, sizeof(s_notice), " tap again to baseline");
                s_notice_until_us = s_baseline_armed_until_us;
                break;
            }
            s_baseline_armed_until_us = 0;
            s_baseline_request = true;
            snprintf(s_notice, sizeof(s_notice), " baseline requested");
            s_notice_until_us = esp_timer_get_time() + 8 * 1000000;
            break;
        case 2: {
            uint8_t top = observore_display_has_light_sensor()
                              ? OBSERVORE_BRIGHT_AUTO : BL_COUNT - 1;
            s_bl_level = (uint8_t)((s_bl_level + 1) % (top + 1));
            bl_apply();
            bl_save();
            break;
        }
    }
    s_dirty = true;
}
#endif

/* Draws. Runs on its own clock so the glass answers a finger while the main
 * loop is inside a thirty-second scan. */
static void ui_task(void *arg)
{
    (void)arg;
    /* As with the touch task: the drawing path formats a page of lines, and
     * the only honest way to size the stack is to read the headroom back. */
    /* Follows the mark down rather than saying it once, the way the heap
     * watch does: the expensive path here is formatting a log line, which
     * only happens when someone is actually touching the panel. Saying it
     * once, before that, is how a stack gets sized wrongly. */
    size_t reported = SIZE_MAX;
    for (;;) {
        size_t spare = uxTaskGetStackHighWaterMark(NULL);
        if (spare + 64 < reported) {
            reported = spare;
            ESP_LOGI(TAG, "%s: %u bytes of %d", "drawing stack headroom",
                     (unsigned)spare, UI_STACK);
        }
#if CONFIG_OBSERVORE_TOUCH
        /* Asked for before the display lock is taken, never while holding it:
         * the touch layer takes the display lock itself, and the two orders
         * together would deadlock. */
        int x = 0, y = 0;
        bool tapped = observore_touch_tap(&x, &y);
#if CONFIG_OBSERVORE_TOUCH_XPT2046
        if (s_cal_step >= 0) {
            /* While calibrating, a press is a measurement rather than a
             * gesture: the mapped coordinates are exactly what is not to be
             * trusted yet, so taps are swallowed here. */
            calibrate_poll();
            if (tapped && esp_timer_get_time() >= s_cal_ready_us) {
                s_cal_step = -1;    /* a tap elsewhere gives up */
                s_dirty = true;
                snprintf(s_notice, sizeof(s_notice), " calibration cancelled");
                s_notice_until_us = esp_timer_get_time() + 5 * 1000000;
            }
            tapped = false;
        }
#endif
#endif
        xSemaphoreTake(s_lock, portMAX_DELAY);
#if CONFIG_OBSERVORE_TOUCH
        if (tapped) {
            /* Every tap, said once, at a level that survives a release build.
             *
             * A tap is rare, deliberate and interesting: on a device meant to
             * sit unattended, somebody touching the glass is arguably a
             * finding in itself. It is also the only way to tell a person
             * from a phantom -- a capacitive panel on battery has no ground
             * reference, and an ungrounded one invents touches. */
            s_taps++;
            ESP_LOGI(TAG, "tap at %d,%d (page %d)", x, y, (int)s_page);
            to_grid_px(&x, &y);
            handle_tap(x, y);
        }
        if (s_pressed >= 0 && esp_timer_get_time() > s_pressed_until_us) {
            s_pressed = -1;
            s_dirty = true;
        }
#endif
#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
        /* While the sensor is in charge, follow the room. Twice a second is
         * far more often than a room changes and still costs one ADC read. */
        if (s_bl_level == OBSERVORE_BRIGHT_AUTO) {
            static int64_t s_last_ldr_us;
            int64_t now_ldr = esp_timer_get_time();
            if (now_ldr - s_last_ldr_us > 500 * 1000) {
                s_last_ldr_us = now_ldr;
                bl_apply();
            }
        }
#endif
        if (s_dirty) {
            s_dirty = false;
            draw_current();
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}

int observore_display_brightness(void)
{
    return s_bl_level;
}

/* What the backlight is actually doing. With the sensor in charge the setting
 * says "auto" and nothing said which level that meant, which makes the one
 * thing worth checking -- is it dimming? -- invisible. */
int observore_display_brightness_effective(void)
{
#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
    if (s_bl_level == OBSERVORE_BRIGHT_AUTO) {
        return s_ldr_chosen;
    }
#endif
    return s_bl_level;
}

bool observore_display_has_light_sensor(void)
{
#if CONFIG_OBSERVORE_DISPLAY_LDR_GPIO >= 0
    return s_ldr_channel >= 0;
#else
    return false;
#endif
}

void observore_display_set_brightness(int step)
{
    int top = observore_display_has_light_sensor() ? OBSERVORE_BRIGHT_AUTO
                                                   : BL_COUNT - 1;
    if (!s_ready || step < 0 || step > top) {
        return;
    }
    s_bl_level = (uint8_t)step;
    bl_apply();
    bl_save();
}

bool observore_display_take_baseline_request(void)
{
    if (!s_ready) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool got = s_baseline_request;
    s_baseline_request = false;
    xSemaphoreGive(s_lock);
    return got;
}

uint32_t observore_display_taps(void)
{
    return s_taps;
}

void observore_display_notice(const char *text, int seconds)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_notice, sizeof(s_notice), " %s", text ? text : "");
    s_notice_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

#else /* no display on this board */

void observore_display_notice(const char *text, int seconds) { (void)text; (void)seconds; }
uint32_t observore_display_taps(void) { return 0; }
void observore_display_init(void) {}
/* No panel: the console hides the control rather than offering one that
 * refuses, which is what a negative level tells it. */
int  observore_display_brightness(void) { return -1; }
int  observore_display_brightness_effective(void) { return -1; }
void observore_display_set_brightness(int step) { (void)step; }
bool observore_display_has_light_sensor(void) { return false; }
void observore_display_backlight_hold(void) {}
void observore_display_backlight_release(void) {}

bool observore_display_take_baseline_request(void) { return false; }
void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us)
{ (void)st; (void)top; (void)n; (void)now_us; }

#endif
