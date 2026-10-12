#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the watch face looks like, as the owner chose it.
 *
 * The face shipped with one appearance and no way to change it, which was
 * right while there was one board with a screen like that and the question was
 * whether a dial could be drawn at all. It is the wrong answer now: a
 * pocket-watch is a thing somebody looks at, and what makes it readable is not
 * the same for everybody. Numerals help if you are reading it quickly and
 * clutter it if you are not; a digital line is either the thing you actually
 * wanted or a blemish on a clean dial.
 *
 * ## Why these are stored rather than built in
 *
 * A preference compiled into the firmware is a preference nobody can have.
 * Changing it would mean a toolchain, and the people this is for are flashing
 * a release from a browser.
 *
 * ## Why the stored form is versioned and checked
 *
 * Because this project has already destroyed two boards' run histories by
 * reading a stored blob back into a struct whose shape had changed, and the
 * error it produced -- INVALID_LENGTH -- was read as "too big" when it meant
 * "too small". So the version leads, the size is asserted at compile time, and
 * anything that does not match is replaced with the defaults rather than
 * interpreted. A face drawn from a misread blob is a face nobody chose.
 *
 * Preferences are also the cheapest possible thing to lose: the cost of
 * discarding them is one tap to set them again, which is nothing next to the
 * cost of drawing something inexplicable.
 */

/* Numerals around the dial. The values are the count deliberately, so the
 * stored number says what it means and a reader does not have to look up an
 * enum -- and so the drawing code can step 12/n hours per numeral. */
typedef enum {
    OBSERVORE_NUMERALS_NONE     = 0,
    OBSERVORE_NUMERALS_QUARTERS = 4,    /* 12, 3, 6, 9 */
    OBSERVORE_NUMERALS_ALL      = 12,
} observore_numerals_t;

#define OBSERVORE_WATCHCFG_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t digital;     /* a time line above the hands */
    uint8_t numerals;    /* one of observore_numerals_t */
    /* 12-hour clock for that line. Zero is 24-hour, which is what the face
     * did before this was a choice -- so the default and an older stored blob
     * both read as the behaviour that already existed, and no migration is
     * needed. */
    uint8_t hour12;
} observore_watchcfg_t;

_Static_assert(sizeof(observore_watchcfg_t) == 4,
               "the stored blob is this struct; its size is the format");

/* Only the digital line changes with hour12. The dial is twelve-hour because
 * it is a dial, and the system page keeps ISO 8601 because that is a timestamp
 * rather than a clock. */

/* The appearance the face shipped with, so turning everything off returns to
 * the dial as it was. */
observore_watchcfg_t observore_watchcfg_default(void);

/* Whether a blob is one this build can draw from: the version matches and
 * every field is a value that means something. Deliberately strict -- see the
 * header note about reading a blob whose shape changed. */
bool observore_watchcfg_valid(const observore_watchcfg_t *cfg);

/* The current preferences. Always safe to draw from: an absent, short or
 * unrecognised blob reads as the defaults. */
observore_watchcfg_t observore_watchcfg_get(void);

/* Store preferences, refusing anything observore_watchcfg_valid rejects, so a
 * caller cannot persist a face that cannot be drawn. */
bool observore_watchcfg_set(const observore_watchcfg_t *cfg);

/* The next setting in each cycle, for a screen where the only gesture is a
 * tap. Pure, so the order a tap walks through is covered by the host tests
 * rather than by trying it. */
uint8_t observore_watchcfg_next_numerals(uint8_t current);
