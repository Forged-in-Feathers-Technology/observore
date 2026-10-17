#include "observore_ultrasound.h"

#include <math.h>
#include <string.h>

/* Spelled out rather than taken from math.h: M_PI is not in C11 and both
 * this project's host suite (-std=c11) and newlib under some configurations
 * leave it undefined. */
#define US_PI 3.14159265358979323846f

/* Goertzel for one frequency over one block.
 *
 * The standard recurrence. `k` is not rounded to an integer bin here: the
 * targets are 250 Hz apart and the block resolves 47 Hz, so the nearest
 * integer bin is within a few hertz of every target, but using the exact
 * ratio costs nothing and keeps the bank's spacing even. */
static float goertzel(const int16_t *x, size_t n, float hz, float rate)
{
    const float w = 2.0f * US_PI * hz / rate;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0, s2 = 0;

    for (size_t i = 0; i < n; i++) {
        /* Scaled to roughly unit range so the squared magnitude of a loud
         * block cannot run away: a full-scale int16 squared and summed over
         * a thousand samples overflows a float's useful precision long
         * before it overflows its range, and the ratios go wrong quietly. */
        float s = (float)x[i] / 32768.0f + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    /* Magnitude squared, normalised by block length so blocks of different
     * sizes are comparable. */
    float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / (float)n;
}

void observore_us_reset(observore_us_t *u)
{
    memset(u, 0, sizeof(*u));
}

int observore_us_feed(observore_us_t *u, const int16_t *samples, size_t n)
{
    if (!u || !samples || n == 0) {
        return -1;
    }

    for (int b = 0; b < OBSERVORE_US_BINS; b++) {
        u->power[b] = goertzel(samples, n, (float)observore_us_bin_hz(b),
                               (float)OBSERVORE_US_RATE);
    }

    /* The first block is the floor rather than a finding.
     *
     * Without this every bin starts at zero, the first block is infinitely
     * above its floor, and the device announces twenty ultrasonic beacons
     * the moment it is switched on. */
    if (!u->seeded) {
        for (int b = 0; b < OBSERVORE_US_BINS; b++) {
            u->floor_[b] = u->power[b] > OBSERVORE_US_MIN_POWER
                               ? u->power[b] : OBSERVORE_US_MIN_POWER;
        }
        u->seeded = true;
        return -1;
    }

    int found = -1;
    for (int b = 0; b < OBSERVORE_US_BINS; b++) {
        /* Two tests, and both must pass. Above the floor this room taught
         * it, *and* above an absolute level -- because a floor learned in
         * silence is zero and everything is infinitely above zero. */
        float floor_ = u->floor_[b] > OBSERVORE_US_MIN_POWER
                           ? u->floor_[b] : OBSERVORE_US_MIN_POWER;
        bool loud = u->power[b] > floor_ * OBSERVORE_US_RATIO &&
                    u->power[b] > OBSERVORE_US_MIN_POWER;

        if (loud) {
            if (u->runs[b] < UINT16_MAX) {
                u->runs[b]++;
            }
            /* Exactly at the threshold, so one episode reports once however
             * long the tone is held. */
            if (u->runs[b] == OBSERVORE_US_RUNS && found < 0) {
                found = b;
            }
        } else {
            u->runs[b] = 0;
            /* The floor only learns from quiet blocks.
             *
             * Learning from every block lets a tone that is held long enough
             * raise its own floor until it stops being a tone -- the detector
             * talks itself out of a beacon that is still sounding, which is
             * the exact failure a persistent emitter would exploit. A slow
             * average over blocks already judged quiet cannot be dragged by
             * the thing it is meant to find. */
            u->floor_[b] += (u->power[b] - u->floor_[b]) / 32.0f;
            if (u->floor_[b] < OBSERVORE_US_MIN_POWER) {
                u->floor_[b] = OBSERVORE_US_MIN_POWER;
            }
        }
    }
    return found;
}
