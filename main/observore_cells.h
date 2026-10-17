/* Sharing a row of character cells among a fixed number of targets.
 *
 * Three strips need this -- the on-screen keyboard, its action bar, and the
 * settings page's actions -- and all three got it wrong the same way, because
 * each worked it out for itself. The obvious form is `COLS / n` for the width
 * and `col / (COLS / n)` for the inverse, and it is wrong twice over: the
 * division truncates, so the remainder is left as dead glass at the
 * right-hand edge or dumped onto the final target, and the inverse does not
 * invert.
 *
 * What that cost, concretely. The keyboard threw away five of the 1.85"
 * panel's thirty-one columns and answered the wrong key on eight of nine in
 * the `asdfghjkl` row. The settings page gave its fourth action
 * `COLS - 3 * (COLS / 4)` columns, which on the same panel is one: the
 * calibrate button was a single character at the screen's edge, and the tap
 * handler -- computing its own divisor, from a different count -- could not
 * return the index that would have pressed it. A button that cannot be seen
 * and cannot be hit.
 *
 * So the arithmetic lives in one place, with one set of tests, and every
 * strip asks it.
 */
#ifndef OBSERVORE_CELLS_H
#define OBSERVORE_CELLS_H

/* Where cell `k` of `n` starts, in character columns across a row of `cols`.
 *
 * Rounded rather than truncated, so the row ends exactly at the right-hand
 * edge and the remainder is spread over the keys a column at a time instead
 * of being left as a dead strip. */
static inline int observore_cell_start(int k, int n, int cols)
{
    return k * cols / n;
}

/* Its width, which is why the two above are separate functions: with the
 * remainder spread, neighbouring keys differ by a column. */
static inline int observore_cell_width(int k, int n, int cols)
{
    return observore_cell_start(k + 1, n, cols) - observore_cell_start(k, n, cols);
}

/* And the inverse: which key a column belongs to.
 *
 * Not col * n / cols, which is the obvious guess and is wrong. That is the
 * inverse of a key boundary at k * cols / n only when n divides cols; once
 * the remainder is spread the two disagree, and a press lands on the key to
 * the left of the one drawn under the finger. At forty columns and nine keys
 * -- the "asdfghjkl" row on both 2.8" boards -- it was wrong for eight of
 * the nine keys.
 *
 * The right answer is the largest k whose start is still at or before col,
 * which solves to ceil((col + 1) * n / cols) - 1.
 *
 * The clamp is for a touch reported at or past the right-hand edge, which
 * both panel drivers can do. */
static inline int observore_cell_at(int col, int n, int cols)
{
    if (col < 0) {
        return 0;
    }
    int k = ((col + 1) * n + cols - 1) / cols - 1;
    if (k < 0) {
        return 0;
    }
    return k >= n ? n - 1 : k;
}

/* How many character columns fit across a round panel, between two rows.
 *
 * The text grid on a round screen is the largest square inside the circle,
 * so its width is set by its corners -- the worst case, at the top and
 * bottom. Anything living in the middle band has more glass either side of
 * it than the grid uses, and the keyboard lives there.
 *
 * `y_top` and `y_bot` bound the band in pixels; the answer is taken at
 * whichever edge is further from the centre line, because that is where the
 * circle is narrowest. Shared with the host suite so the sizes it reports
 * are the sizes that ship: it used to measure the grid's width while the
 * firmware drew at the band's, which made the test pessimistic -- safe, but
 * not a measurement of anything real.
 *
 * Returns 0 when the band does not fit at all. */
static inline int observore_chord_cols(int diameter, int y_top, int y_bot,
                                       int font_w)
{
    const int r = diameter / 2;
    const int cy = diameter / 2;
    int dt = y_top > cy ? y_top - cy : cy - y_top;
    int db = y_bot > cy ? y_bot - cy : cy - y_bot;
    int dy = dt > db ? dt : db;
    if (dy >= r || font_w <= 0) {
        return 0;
    }
    /* Integer square root of r^2 - dy^2: the half-chord there. */
    const int want = r * r - dy * dy;
    int half = 0;
    while ((half + 1) * (half + 1) <= want) {
        half++;
    }
    return (2 * half) / font_w;
}

#endif /* OBSERVORE_CELLS_H */
