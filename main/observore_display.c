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
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "observore_netcfg.h"
#include "observore_nvs.h"
#include "observore_runs.h"
#include "observore_wifi.h"
#include "observore_touch.h"
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

static const char *TAG = "observore.display";

/* Landscape. The panel is 240x320 portrait; swapping axes gives 320 wide. */
#define DISP_W OBSERVORE_DISPLAY_W
#define DISP_H OBSERVORE_DISPLAY_H
#define COLS   (DISP_W / OBSERVORE_FONT_W)   /* 40 */
#define ROWS   (DISP_H / OBSERVORE_FONT_H)   /* 15 */

/* RGB565. Chosen to read across a room, not to be pretty. */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
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


static esp_lcd_panel_handle_t s_panel;
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

/* The panel takes each 16-bit pixel most significant byte first and the SPI
 * path sends the host's bytes as they are, so the swap is done here. Measured
 * on the bench rather than taken from the data_endian field: without this the
 * green band came out red, and with inversion on top it came out cyan. */
static inline uint16_t px(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

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
    int x0 = col * OBSERVORE_FONT_W, y0 = row * OBSERVORE_FONT_H;
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
/* Both of these were set generously and then measured. The drawing task
 * formats one forty-column line at a time and calls into the panel driver;
 * the snapshot only has to hold what a page can show, which is eleven rows.
 * On the board with a screen there is no PSRAM, and every kilobyte held here
 * is one the TLS handshake behind an update check cannot have -- which is how
 * v0.8.2 shipped a CYD that could no longer check for its own updates. */
/* Measured, not chosen: drawing the keyboard leaves 536 bytes spare at 2560. */
#define UI_STACK     3072
#define SNAP_MAX     8

static SemaphoreHandle_t s_lock;
static observore_status_t s_snap_st;
static observore_event_t  s_snap_top[SNAP_MAX];
static size_t             s_snap_n;
static int64_t            s_snap_now_us;
static bool               s_have_snap;
static bool               s_dirty;

/* Which page, and which button is showing as pressed. */
typedef enum { PAGE_WATCH = 0, PAGE_SYSTEM,
#if CONFIG_OBSERVORE_TOUCH
               PAGE_WIFI,
#endif
               PAGE_COUNT } page_t;
static page_t  s_page;
static int     s_pressed = -1;        /* button index, or -1 */
static int64_t s_pressed_until_us;
static bool    s_baseline_request;

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
#define KB_TOP     3
#define KB_HEIGHT  2
#define ACTION_ROW (KB_TOP + KEY_ROWS * KB_HEIGHT)   /* 11 */
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
static uint8_t s_bl_level;   /* index into BL_LEVELS */

static void ui_task(void *arg);

static void bl_apply(void)
{
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }
    ledc_set_duty(BL_MODE, BL_CHANNEL, BL_LEVELS[s_bl_level]);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
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
    if (CONFIG_OBSERVORE_DISPLAY_BL < 0) {
        return;
    }

    uint8_t v = 0;
    observore_nvs_item_t item = {.key = "bright", .type = OBSERVORE_NVS_BLOB,
                                 .buf = &v, .len = sizeof(v)};
    if (observore_nvs_read(&item, 1) == ESP_OK && item.found &&
        v < BL_COUNT) {
        s_bl_level = v;
    }

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
}

void observore_display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no memory for the display lock");
        return;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_OBSERVORE_DISPLAY_MOSI,
        .miso_io_num = CONFIG_OBSERVORE_DISPLAY_MISO,
        .sclk_io_num = CONFIG_OBSERVORE_DISPLAY_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_cells[0]),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_OBSERVORE_DISPLAY_CS,
        .dc_gpio_num = CONFIG_OBSERVORE_DISPLAY_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_OBSERVORE_DISPLAY_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_OBSERVORE_DISPLAY_RST,
        .rgb_ele_order = CONFIG_OBSERVORE_DISPLAY_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                                      : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
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
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, CONFIG_OBSERVORE_DISPLAY_MIRROR_X,
                         CONFIG_OBSERVORE_DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);


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
    ESP_LOGI(TAG, PANEL_NAME " %dx%d, %d columns", DISP_W, DISP_H, COLS);
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
    snprintf(text, sizeof(text), "  %u device%s   %lu sightings",
             st->device_count, st->device_count == 1 ? "" : "s",
             (unsigned long)st->total_sightings);
    line(1, text, C_BLACK, band);

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
    for (size_t i = 0; i < n && row < last_row; i++, row++) {
        char mac[OBSERVORE_MAC_STR_LEN];
        observore_mac_str(top[i].mac, mac);
        const char *who = top[i].detail[0] ? top[i].detail
                        : top[i].vendor    ? top[i].vendor
                        : top[i].addr_random ? "random" : "";
        snprintf(text, sizeof(text), "%-8.8s %s %4d %.8s",
                 observore_class_name(top[i].cls), mac, top[i].rssi, who);
        uint16_t fg = top[i].cls == OBSERVORE_CLASS_FOLLOWER ? C_WHITE : C_AMBER;
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
static void draw_system(const observore_status_t *st, int64_t now_us)
{
    char text[COLS + 1], up[16];
    const esp_app_desc_t *app = esp_app_get_description();

    line(0, "  OBSERVORE  system", C_BLACK, C_GREY);
    snprintf(text, sizeof(text), " version  %.28s", app ? app->version : "?");
    line(1, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " board    %.28s", CONFIG_OBSERVORE_BOARD);
    line(2, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " up       %.28s", ago(now_us, up, sizeof(up)));
    line(3, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " seen     %u device%s, %lu sightings",
             st->device_count, st->device_count == 1 ? "" : "s",
             (unsigned long)st->total_sightings);
    line(4, text, C_WHITE, C_BLACK);
    snprintf(text, sizeof(text), " heap     %u free, %u least",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    line(5, text, C_WHITE, C_BLACK);
    line(6, "", C_WHITE, C_BLACK);
    line(7, " previous runs", C_GREY, C_BLACK);

    observore_run_t runs[4];
    size_t nr = observore_runs_list(runs, 4);
    int row = 8;
    for (size_t i = 0; i < nr && row < ROWS - BAR_LAST; i++, row++) {
        char dur[16];
        snprintf(dur, sizeof(dur), "%lluh%02llum",
                 (unsigned long long)(runs[i].up_s / 3600),
                 (unsigned long long)((runs[i].up_s % 3600) / 60));
        snprintf(text, sizeof(text), "   %-8.8s ended by %.18s", dur,
                 observore_reset_reason_name((esp_reset_reason_t)runs[i].end));
        line(row, text, C_WHITE, C_BLACK);
    }
    if (nr == 0) {
        line(row++, "   none recorded yet", C_GREY, C_BLACK);
    }
    for (; row < ROWS - BAR_LAST; row++) {
        line(row, "", C_WHITE, C_BLACK);
    }
}

/* The button bar, on the bottom row. Three targets across forty columns: a
 * person with a fingertip and a resistive panel needs them wide. */
#define BUTTONS 3
static const char *BUTTON_TEXT[BUTTONS] = {"  page", "  baseline", "  light"};

/* The bar is drawn two rows deep and answers to the bottom three.
 *
 * A single text row is sixteen pixels, about two millimetres: smaller than a
 * fingertip, at the edge of the glass where a resistive sheet is least
 * accurate, and on this board partly under the lip of a case. The margin above
 * the drawn bar is what makes it hittable without looking. */
#define BAR_ROWS 2
#define BAR_TOUCH_ROWS 3

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
            pos += snprintf(bot + pos, sizeof(bot) - pos, " %c ", ch);
        }
        bot[pos] = '\0';
        memset(top, ' ', (size_t)pos); top[pos] = '\0';
        line(KB_TOP + kr * KB_HEIGHT,     top, C_WHITE, C_DARK);
        line(KB_TOP + kr * KB_HEIGHT + 1, bot, C_WHITE, C_DARK);
    }

    /* Four actions across the width, ten columns each. */
    snprintf(text, sizeof(text), "%-10.10s%-10.10s%-10.10s%-10.10s",
             s_shift ? "   abc" : "   ABC#", reveal ? "   hide" : "   show",
             "   del", "   join");
    line(ACTION_ROW, text, C_BLACK, C_GREY);
    for (int r = ACTION_ROW + 1; r < ROWS - BAR_LAST; r++) {
        line(r, "", C_WHITE, C_BLACK);
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
        int kc = (x / OBSERVORE_FONT_W) / 3;
        if (kr < KEY_ROWS && kc < KEY_COLS) {
            char ch = KEYS[s_shift ? 1 : 0][kr][kc];
            if (ch && ch != ' ' && s_pass_len + 1 < sizeof(s_pass)) {
                s_pass[s_pass_len++] = ch;
                s_pass[s_pass_len]   = '\0';
            }
        }
        return;
    }
    if (row == ACTION_ROW) {
        int which = (x / OBSERVORE_FONT_W) / 10;
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
#if CONFIG_OBSERVORE_TOUCH
    if (s_page == PAGE_WIFI) {
        draw_wifi();
    } else
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
/* One tap. The bar is the only thing that takes input: the pages above it are
 * read, not operated, and a stray touch on a reading should do nothing. */
static void handle_tap(int x, int y)
{
    int b = button_at(x, y);
    if (b < 0) {
        /* Above the bar. Only the network page takes input there; the others
         * are read, not operated, and a stray touch on a reading does
         * nothing. */
        if (s_page == PAGE_WIFI) {
            wifi_tap(x, y);
            s_dirty = true;
        }
        return;
    }
    s_pressed = b;
    s_pressed_until_us = esp_timer_get_time() + 200 * 1000;

    switch (b) {
        case 0:
            s_page = (s_page + 1) % PAGE_COUNT;
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
            s_baseline_request = true;
            snprintf(s_notice, sizeof(s_notice), " baseline requested");
            s_notice_until_us = esp_timer_get_time() + 8 * 1000000;
            break;
        case 2:
            s_bl_level = (s_bl_level + 1) % BL_COUNT;
            bl_apply();
            bl_save();
            break;
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
#endif
        xSemaphoreTake(s_lock, portMAX_DELAY);
#if CONFIG_OBSERVORE_TOUCH
        if (tapped) {
            handle_tap(x, y);
        }
        if (s_pressed >= 0 && esp_timer_get_time() > s_pressed_until_us) {
            s_pressed = -1;
            s_dirty = true;
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

void observore_display_set_brightness(int step)
{
    if (!s_ready || step < 0 || step >= BL_COUNT) {
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
void observore_display_init(void) {}
/* No panel: the console hides the control rather than offering one that
 * refuses, which is what a negative level tells it. */
int  observore_display_brightness(void) { return -1; }
void observore_display_set_brightness(int step) { (void)step; }
void observore_display_backlight_hold(void) {}
void observore_display_backlight_release(void) {}

bool observore_display_take_baseline_request(void) { return false; }
void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us)
{ (void)st; (void)top; (void)n; (void)now_us; }

#endif
