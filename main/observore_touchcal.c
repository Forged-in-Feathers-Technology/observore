#include "observore_touchcal.h"

/* One raw axis: the readings at the two targets, how far apart those targets
 * were along the screen axis this channel travels, and how long that screen
 * axis is in total. */
static bool solve_axis(int r0, int r1, int target_sep, int full, int *lo, int *hi)
{
    int span = r1 - r0;
    int mag  = span < 0 ? -span : span;
    if (mag < OBSERVORE_TOUCHCAL_MIN_SPAN) {
        return false;
    }
    if (target_sep <= 0 || full <= 1) {
        return false;
    }

    /* What the whole sheet would read, if the sheet is linear: the measured
     * span covered `target_sep` pixels, and the sheet covers `full - 1`. */
    int full_span = (int)((long long)span * (full - 1) / target_sep);
    int pad = (full_span - span) / 2;

    int a = r0 - pad, b = r1 + pad;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
    return true;
}

bool observore_touchcal_solve(int raw_x0, int raw_y0, int raw_x1, int raw_y1,
                              int sx0, int sy0, int sx1, int sy1,
                              int disp_w, int disp_h, bool swap,
                              observore_touchcal_t *out)
{
    if (!out) {
        return false;
    }

    int dx = sx1 - sx0; if (dx < 0) { dx = -dx; }
    int dy = sy1 - sy0; if (dy < 0) { dy = -dy; }

    /* Pair each raw channel with the screen axis it actually runs along. This
     * is the whole point of the file: getting it backwards is not a small
     * error, because the two axes of these panels have very different aspect
     * ratios and the inset is a different fraction of each. */
    int x_sep, x_full, y_sep, y_full;
    if (swap) {
        x_sep = dy; x_full = disp_h;      /* raw X travels down the screen */
        y_sep = dx; y_full = disp_w;      /* raw Y travels across it */
    } else {
        x_sep = dx; x_full = disp_w;
        y_sep = dy; y_full = disp_h;
    }

    observore_touchcal_t got;
    if (!solve_axis(raw_x0, raw_x1, x_sep, x_full, &got.lo_x, &got.hi_x)) {
        return false;
    }
    if (!solve_axis(raw_y0, raw_y1, y_sep, y_full, &got.lo_y, &got.hi_y)) {
        return false;
    }

    *out = got;
    return true;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool observore_touchcal_fit(observore_touchcal_t *c, int raw_max)
{
    if (!c || raw_max <= OBSERVORE_TOUCHCAL_MIN_SPAN) {
        return false;
    }
    if (c->hi_x < c->lo_x || c->hi_y < c->lo_y) {
        return false;
    }
    observore_touchcal_t fitted = {
        .lo_x = clamp(c->lo_x, 0, raw_max), .hi_x = clamp(c->hi_x, 0, raw_max),
        .lo_y = clamp(c->lo_y, 0, raw_max), .hi_y = clamp(c->hi_y, 0, raw_max),
    };
    /* Clamping can only narrow a span, so the floor is re-checked afterwards:
     * bounds that were mostly outside the range come back as a sliver. */
    if (fitted.hi_x - fitted.lo_x < OBSERVORE_TOUCHCAL_MIN_SPAN ||
        fitted.hi_y - fitted.lo_y < OBSERVORE_TOUCHCAL_MIN_SPAN) {
        return false;
    }
    *c = fitted;
    return true;
}
