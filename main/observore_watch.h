#pragma once

#include <stdint.h>

/* Drawing a watch face.
 *
 * The rest of this project renders an 8x16 monospace grid straight to the
 * panel, which is right for a page of readings and cannot draw a diagonal.
 * A dial needs lines at arbitrary angles, so this draws into a buffer the
 * caller owns and blits once -- affordable on exactly one board, which has
 * eight megabytes of PSRAM and a 466-pixel circle to fill.
 *
 * No ESP-IDF in here: it is geometry, and the host tests check the angles
 * rather than somebody squinting at a screen.
 *
 * Colours are passed in the panel's own byte order, already swapped by the
 * caller, because that is a property of the wiring rather than of a dial. */

typedef struct {
    uint16_t *px;
    int       w;
    int       h;
} observore_canvas_t;

void observore_watch_fill(const observore_canvas_t *c, uint16_t colour);

/* A line, `width` pixels thick, clipped to the canvas. */
void observore_watch_line(const observore_canvas_t *c, int x0, int y0,
                          int x1, int y1, int width, uint16_t colour);

/* A circle outline of the given thickness, and a filled one. */
void observore_watch_ring(const observore_canvas_t *c, int cx, int cy,
                          int r, int thickness, uint16_t colour);
void observore_watch_disc(const observore_canvas_t *c, int cx, int cy,
                          int r, uint16_t colour);

/* Where a hand points: `units` out of `per_rev` around the dial, clockwise
 * from twelve o'clock, `len` pixels from the centre.
 *
 * Separated out and tested because an off-by-a-quarter-turn here is the
 * classic way to ship a clock whose hands are ninety degrees out, and it
 * looks plausible enough in a photograph to survive review. */
void observore_watch_hand_end(int cx, int cy, int len, int units, int per_rev,
                              int *x, int *y);

/* A watch hand: tapered from the hub to a point, with a counterweight
 * behind the centre.
 *
 * A bare line from the middle to the rim reads as a diagram. The taper and
 * the tail are most of what makes a dial look like a watch, and they cost a
 * handful of segments. */
void observore_watch_hand(const observore_canvas_t *c, int cx, int cy,
                          int tipx, int tipy, int tail, int w_hub, int w_tip,
                          uint16_t colour);

/* One character from the project's font, scaled by an integer factor. */
void observore_watch_glyph(const observore_canvas_t *c, int x, int y, char ch,
                           int scale, uint16_t colour);

/* A string, centred on x. Returns the width it drew. */
int observore_watch_text(const observore_canvas_t *c, int cx, int y,
                         const char *s, int scale, uint16_t colour);
