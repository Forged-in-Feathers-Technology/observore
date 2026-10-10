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
/* `ch` is unsigned: the font runs past printable ASCII into icons, and a
 * signed char would turn the second of them into a negative number that fails
 * the range check. The compiler found this one -- raising the table's last
 * code to 0x7F made `ch > LAST` always false for a signed char, which
 * -Werror=type-limits refused. The same trap was in the panel's draw_glyph. */
void observore_watch_glyph(const observore_canvas_t *c, int x, int y, uint8_t ch,
                           int scale, uint16_t colour);

/* A string, centred on x. Returns the width it drew. */
int observore_watch_text(const observore_canvas_t *c, int cx, int y,
                         const char *s, int scale, uint16_t colour);

/* Where to put the dial's centre on step `step`, as an offset from the middle
 * of the panel.
 *
 * An AMOLED ages where it is lit. The dial is the one thing on this device
 * that holds still indefinitely -- a bright silver ring and twelve markers in
 * the same pixels for as long as the device is on -- so the whole face walks
 * slowly around a circle of `radius` pixels and no pixel keeps a bright
 * element for long. Eight positions, one per step.
 *
 * The caller shrinks the dial by `radius` to pay for the room. Here rather
 * than in the display because it is arithmetic, and because what needs
 * checking is that every step stays inside the glass. */
void observore_watch_shift(unsigned step, int radius, int *dx, int *dy);

/* The largest dial radius that still fits, given the panel, the margin the
 * design wants, the thickness of the ring and how far the face walks.
 *
 * This is one line of arithmetic and it lives here so that a host test can
 * check the thing that actually matters: that the ring is inside the glass
 * at every step of the walk. Left in the display it would be untestable, and
 * "subtract the shift as well" is exactly the term somebody adds a walk
 * without. */
int observore_watch_dial_radius(int panel_w, int panel_h, int margin,
                                int ring, int shift);
