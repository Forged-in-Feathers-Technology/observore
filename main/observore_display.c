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

#include "observore_detect.h"
#include "observore_font.h"
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
#define DISP_W 320
#define DISP_H 240
#define COLS   (DISP_W / OBSERVORE_FONT_W)   /* 40 */
#define ROWS   (DISP_H / OBSERVORE_FONT_H)   /* 15 */

/* RGB565. Chosen to read across a room, not to be pretty. */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
#define C_GREEN  0x07E0
#define C_AMBER  0xFD20
#define C_RED    0xF800

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

void observore_display_init(void)
{
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

    /* Backlight last, so nobody watches the panel initialise. */
    if (CONFIG_OBSERVORE_DISPLAY_BL >= 0) {
        gpio_config_t bl = {
            .pin_bit_mask = 1ULL << CONFIG_OBSERVORE_DISPLAY_BL,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bl);
    }

    memset(s_shown, 0, sizeof(s_shown));
    s_ready = true;
    for (int r = 0; r < ROWS; r++) {
        line(r, "", C_WHITE, C_BLACK);
    }
    line(0, "  OBSERVORE", C_BLACK, C_GREY);
    line(1, "  starting", C_BLACK, C_GREY);
    if (CONFIG_OBSERVORE_DISPLAY_BL >= 0) {
        gpio_set_level(CONFIG_OBSERVORE_DISPLAY_BL, 1);
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

void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us)
{
    if (!s_ready || !st) {
        return;
    }
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
    const int last_row = ROWS - 1;
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

    char up[16];
    const esp_app_desc_t *app = esp_app_get_description();
    /* Truncation of the version is fine and deliberate: "v0.7.0-1-gb59a6d4"
     * cut short still identifies the build, and the row has forty columns. */
    snprintf(text, sizeof(text), " up %.10s   %.20s", ago(now_us, up, sizeof(up)),
             app ? app->version : "");
    line(last_row, text, C_GREY, C_BLACK);
}

void observore_display_notice(const char *text, int seconds)
{
    snprintf(s_notice, sizeof(s_notice), " %s", text ? text : "");
    s_notice_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
}

#else /* no display on this board */

void observore_display_notice(const char *text, int seconds) { (void)text; (void)seconds; }
void observore_display_init(void) {}
void observore_display_render(const observore_status_t *st,
                              const observore_event_t *top, size_t n,
                              int64_t now_us)
{ (void)st; (void)top; (void)n; (void)now_us; }

#endif
