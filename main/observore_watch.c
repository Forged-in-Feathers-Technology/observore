#include "observore_watch.h"

#include <stdlib.h>
#include <string.h>

#include "observore_font.h"

/* Sine of the dial angle, in units of 1/10000, for 0..90 degrees at one
 * degree steps. A table rather than floating point: this runs once a second
 * for the life of the device, and a watch face does not need more than a
 * tenth of a pixel of accuracy at this radius. */
static const int16_t SIN_Q[91] = {
       0,  175,  349,  523,  698,  872, 1045, 1219, 1392, 1564,
    1736, 1908, 2079, 2250, 2419, 2588, 2756, 2924, 3090, 3256,
    3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848,
    5000, 5150, 5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293,
    6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314, 7431, 7547,
    7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572,
    8660, 8746, 8829, 8910, 8988, 9063, 9135, 9205, 9272, 9336,
    9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
    9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998,
   10000,
};

static int sin_q(int deg)
{
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 90)  { return SIN_Q[deg]; }
    if (deg <= 180) { return SIN_Q[180 - deg]; }
    if (deg <= 270) { return -SIN_Q[deg - 180]; }
    return -SIN_Q[360 - deg];
}

static int cos_q(int deg) { return sin_q(deg + 90); }

static inline void put(const observore_canvas_t *c, int x, int y, uint16_t colour)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) {
        return;
    }
    c->px[(size_t)y * (size_t)c->w + (size_t)x] = colour;
}

void observore_watch_fill(const observore_canvas_t *c, uint16_t colour)
{
    size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t i = 0; i < n; i++) {
        c->px[i] = colour;
    }
}

void observore_watch_line(const observore_canvas_t *c, int x0, int y0,
                          int x1, int y1, int width, uint16_t colour)
{
    /* Bresenham, thickened by stamping a small square at each step. Good
     * enough for a hand two or three pixels wide, and it costs no division. */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = width / 2;
    for (;;) {
        for (int oy = -half; oy <= half; oy++) {
            for (int ox = -half; ox <= half; ox++) {
                put(c, x0 + ox, y0 + oy, colour);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void observore_watch_disc(const observore_canvas_t *c, int cx, int cy,
                          int r, uint16_t colour)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                put(c, cx + x, cy + y, colour);
            }
        }
    }
}

void observore_watch_ring(const observore_canvas_t *c, int cx, int cy,
                          int r, int thickness, uint16_t colour)
{
    int inner = r - thickness;
    if (inner < 0) {
        inner = 0;
    }
    int r2 = r * r, i2 = inner * inner;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r2 && d >= i2) {
                put(c, cx + x, cy + y, colour);
            }
        }
    }
}

void observore_watch_hand_end(int cx, int cy, int len, int units, int per_rev,
                              int *x, int *y)
{
    /* Clockwise from twelve, so the angle grows with the units and twelve is
     * straight up -- which on a screen means negative y. */
    int deg = per_rev > 0 ? (units * 360) / per_rev : 0;
    if (x) { *x = cx + (len * sin_q(deg)) / 10000; }
    if (y) { *y = cy - (len * cos_q(deg)) / 10000; }
}

void observore_watch_hand(const observore_canvas_t *c, int cx, int cy,
                          int tipx, int tipy, int tail, int w_hub, int w_tip,
                          uint16_t colour)
{
    /* Walked in segments from a point behind the centre out to the tip, each
     * a little narrower than the last. Integer throughout: the widths are
     * single digits and a watch hand does not need sub-pixel edges. */
    const int steps = 12;
    int dx = tipx - cx, dy = tipy - cy;

    /* The counterweight, opposite the tip. */
    if (tail > 0) {
        int len = 1;
        int mag = dx * dx + dy * dy;
        while (len * len < mag) {
            len++;          /* integer length, near enough for a direction */
        }
        if (len > 0) {
            int bx = cx - (dx * tail) / len;
            int by = cy - (dy * tail) / len;
            observore_watch_line(c, cx, cy, bx, by, w_hub, colour);
        }
    }

    for (int i = 0; i < steps; i++) {
        int x0 = cx + (dx * i) / steps;
        int y0 = cy + (dy * i) / steps;
        int x1 = cx + (dx * (i + 1)) / steps;
        int y1 = cy + (dy * (i + 1)) / steps;
        int w  = w_hub - ((w_hub - w_tip) * i) / steps;
        observore_watch_line(c, x0, y0, x1, y1, w, colour);
    }
}

void observore_watch_glyph(const observore_canvas_t *c, int x, int y, char ch,
                           int scale, uint16_t colour)
{
    if (ch < OBSERVORE_FONT_FIRST || ch > OBSERVORE_FONT_LAST) {
        ch = '?';
    }
    const uint8_t *g = OBSERVORE_FONT[ch - OBSERVORE_FONT_FIRST];
    for (int row = 0; row < OBSERVORE_FONT_H; row++) {
        for (int col = 0; col < OBSERVORE_FONT_W; col++) {
            if (!((g[row] >> (7 - col)) & 1)) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    put(c, x + col * scale + sx, y + row * scale + sy, colour);
                }
            }
        }
    }
}

int observore_watch_text(const observore_canvas_t *c, int cx, int y,
                         const char *s, int scale, uint16_t colour)
{
    int n = (int)strlen(s);
    int w = n * OBSERVORE_FONT_W * scale;
    int x = cx - w / 2;
    for (int i = 0; i < n; i++) {
        observore_watch_glyph(c, x + i * OBSERVORE_FONT_W * scale, y, s[i],
                              scale, colour);
    }
    return w;
}

int observore_watch_dial_radius(int panel_w, int panel_h, int margin,
                                int ring, int shift)
{
    int half = (panel_w < panel_h ? panel_w : panel_h) / 2;
    /* The ring is drawn centred on the radius, so half of it sticks out. */
    int r = half - margin - shift - (ring + 1) / 2;
    return r > 0 ? r : 0;
}

void observore_watch_shift(unsigned step, int radius, int *dx, int *dy)
{
    /* Eight points on a circle, as sin and cos times a thousand, so the walk
     * is integer arithmetic and the same on the host as on the device.
     *
     * A circle rather than a raster scan because consecutive positions are
     * then adjacent: the dial moves about two pixels per step and never
     * jumps across the face. A scan would snap from one edge to the other
     * once a cycle, which is the one moment somebody would notice. */
    static const int sin1000[8] = {0, 707, 1000,  707,    0, -707, -1000, -707};
    static const int cos1000[8] = {1000, 707,  0, -707, -1000, -707,  0,   707};

    if (radius < 0) {
        radius = 0;
    }
    int i = (int)(step & 7u);

    /* Rounded to nearest rather than truncated. At radius 3 the diagonals are
     * 2.12 pixels, and truncation would make them 2 while the axes stay 3 --
     * a visibly lopsided walk at the only radius this is used at. */
    int sx = radius * sin1000[i];
    int sy = radius * cos1000[i];
    *dx =  (sx >= 0 ? sx + 500 : sx - 500) / 1000;
    *dy = -((sy >= 0 ? sy + 500 : sy - 500) / 1000);
}
