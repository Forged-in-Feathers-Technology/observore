#include "observore_tap.h"

#include <string.h>

void observore_tap_reset(observore_tap_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

bool observore_tap_sample(observore_tap_state_t *s, bool down, int x, int y,
                          int64_t now_us, int64_t release_us,
                          int *tap_x, int *tap_y)
{
    if (!s) {
        return false;
    }

    if (down) {
        if (!s->down) {
            s->down    = true;
            s->down_x  = x;
            s->down_y  = y;
            s->down_us = now_us;
        }
        /* Contact again. Whatever gap was being timed was a dip in the
         * pressure reading rather than the finger leaving, so the press
         * continues and the position it started at is kept -- a press that
         * flickers is still one press, at one place. */
        s->lost_us = 0;
        return false;
    }

    if (!s->down) {
        return false;          /* nothing happening, and nothing to decide */
    }

    /* Contact missing while a press is in progress. Note when it went, and
     * wait: on a board with no interrupt line the pressure reading is the only
     * witness there is, and one 20 ms sample is a thin basis for ending a
     * press that a finger is still making. */
    if (s->lost_us == 0) {
        s->lost_us = now_us;
    }
    if (now_us - s->lost_us < release_us) {
        return false;
    }

    /* The press is over. Its length is measured to where contact was lost,
     * not to now: the time spent waiting to be sure is not time a finger was
     * on the glass, and counting it would let the release window itself push
     * a too-brief contact over the minimum. */
    bool deliberate = (s->lost_us - s->down_us) >= OBSERVORE_TAP_MIN_CONTACT_US;

    if (deliberate && tap_x && tap_y) {
        *tap_x = s->down_x;
        *tap_y = s->down_y;
    }
    s->down    = false;
    s->lost_us = 0;
    return deliberate;
}
