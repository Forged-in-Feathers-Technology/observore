/* The on-screen keyboard's layout and geometry.
 *
 * Here rather than in observore_display.c so the host suite can measure it.
 * What went wrong with the first version was arithmetic, not drawing, and
 * arithmetic can be checked without a panel.
 *
 * Measured on the five boards that have a touch screen, the keys were:
 *
 *   cyd-2432s028r  2.8"   40 cols   24 px   4.3 mm
 *   nm-cyd-c5      2.8"   40 cols   24 px   4.3 mm
 *   cyd-3248s035r  3.5"   60 cols   32 px   4.9 mm
 *   amoled-143     round  41 cols   24 px   1.9 mm
 *   lcd-185        round  31 cols   16 px   2.1 mm
 *
 * against the seven millimetres the code's own comment claimed. A fingertip
 * covers six to eight, so on the round boards a press landed across three or
 * four keys and the panel picked whichever its centroid fell in. That is not
 * a touch fault -- those panels report accurately -- it is a target the size
 * of the gap between two keys.
 *
 * Three things made it small, and two of them were ours:
 *
 *  - the width was COLS/KEY_COLS, which truncates. At 31 columns and 13 keys
 *    that is 2 instead of 2.38: five of the panel's thirty-one columns, a
 *    sixth of the glass, went unused at the right-hand edge.
 *
 *  - the grid was thirteen wide because one row of symbols needed thirteen.
 *    The letter rows need ten, nine and seven, and were padded out with dead
 *    cells that still took up width.
 *
 *  - a round panel packs its pixels into much less glass: the 1.43" fits 466
 *    of them across 36 mm, nearly three per millimetre of what the 2.8" board
 *    manages. Nothing in the layout knew that, and nothing here can fix it --
 *    the glass is the size it is. The other two are arithmetic.
 *
 * So rows are ragged and keys share the row's whole width. A row of seven
 * keys gets wider ones than a row of ten, which is also how a thumb keyboard
 * has always been drawn.
 */
#ifndef OBSERVORE_KEYBOARD_H
#define OBSERVORE_KEYBOARD_H

#include "observore_cells.h"

#define OBSERVORE_KB_ROWS   4
#define OBSERVORE_KB_LAYERS 3

/* Lower, upper, symbols.
 *
 * Three layers rather than two because the symbols are what forced the
 * thirteen-wide grid, and they are the rarest thing in a passphrase -- one or
 * two characters out of twenty. Giving them a layer of their own buys every
 * letter on both other layers a third more width.
 *
 * Every position is a key; there are no gaps, which is what lets the final
 * letter row hold a space. There was no way to type one before, and a WPA
 * passphrase is allowed to contain them -- "my home network" could be entered
 * from a browser and not from the screen. It draws as an underscore.
 *
 * The digits sit on both letter layers so that shifting to reach a capital
 * does not take them away.
 *
 * The symbol layer is the thirty-two punctuation characters of printable
 * ASCII, all of them, eight to a row: anything a router's setup page would
 * accept in a passphrase can be typed here. */
static const char *const OBSERVORE_KB[OBSERVORE_KB_LAYERS][OBSERVORE_KB_ROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm " },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM " },
    { "!@#$%^&*",   "()-_=+[]",   "{}|\\:;'\"", ",.<>?/`~" },
};

#endif /* OBSERVORE_KEYBOARD_H */
