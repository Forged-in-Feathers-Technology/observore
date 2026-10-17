#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Looking for inaudible tones: ultrasonic cross-device tracking.
 *
 * ## What this is for
 *
 * Some advertising and retail-analytics systems emit a short tone just above
 * hearing -- around 18 to 20 kHz -- from a shop display, a television advert
 * or a web page, and listen for it on a phone whose app holds the microphone
 * permission. Hearing the same tone links the two devices: the phone in your
 * pocket and the screen you walked past, or the TV in your front room. It is
 * a surveillance channel that works precisely because nobody can hear it.
 *
 * Observore watches radios. This is the same job in a different medium, and
 * it is the one surveillance channel this hardware could already see and was
 * not looking at.
 *
 * ## What it deliberately is not
 *
 * It never records, never buffers audio beyond the block being analysed, and
 * never sends a sample anywhere. A counter-surveillance device that keeps a
 * microphone open is a listening device, and the fact that it means well is
 * not a property a person can verify from outside. What leaves this module
 * is a handful of energies per band -- numbers, from which no speech can be
 * recovered.
 *
 * That is also why the band starts above speech. The bins begin at 17 kHz,
 * which is above the top of the voice and above almost all of music; the
 * analysis is structurally incapable of hearing a conversation because the
 * frequencies a conversation lives in are never computed.
 *
 * ## Why a Goertzel bank and not an FFT
 *
 * An FFT would give the whole spectrum, 24 kHz of it, when the question only
 * concerns the top 5. Goertzel evaluates one frequency at a time for about
 * the cost of a multiply-accumulate per sample, so a bank of twenty costs a
 * fraction of a transform and brings in no component: esp-dsp would be a new
 * managed dependency resolved for all ten board profiles, for arithmetic
 * that is twenty lines.
 *
 * The analysis is here, separate from the microphone, so the host suite can
 * feed it synthetic tones and check what it reports.
 */

/* 17 kHz upward, in 250 Hz steps. Twenty bins reaches 21.75 kHz, past every
 * documented beacon scheme and short of the 24 kHz ceiling a 48 kHz sample
 * rate imposes. */
#define OBSERVORE_US_BINS      20
#define OBSERVORE_US_BASE_HZ   17000
#define OBSERVORE_US_STEP_HZ   250
#define OBSERVORE_US_RATE      48000

/* One analysis block: 960 samples at 48 kHz, which is 20 ms.
 *
 * 960 and not a round 1024, and the difference is the whole selectivity of
 * the thing. A rectangular-window Goertzel is sharp only when its target
 * falls on an exact bin of the block -- when N*f/Fs is a whole number. At
 * N=1024 the 17 kHz target lands on bin 362.67 and every step is 5.33 bins
 * apart, so each filter leaks badly across the band. Measured with 1024: a
 * loud 8 kHz tone, far below the band, was reported as a beacon.
 *
 * At N=960 every 250 Hz step is exactly 5 bins and 17 kHz is exactly bin
 * 340. No scalloping, and the leakage from a tone outside the band falls
 * away as it should. */
#define OBSERVORE_US_BLOCK     960

typedef struct {
    /* Energy per bin for the block just analysed, and the learned quiet
     * level each is judged against. */
    float power[OBSERVORE_US_BINS];
    float floor_[OBSERVORE_US_BINS];
    /* How many consecutive blocks each bin has stood above its floor. */
    uint16_t runs[OBSERVORE_US_BINS];
    bool  seeded;
} observore_us_t;

/* The centre frequency of a bin. */
static inline int observore_us_bin_hz(int bin)
{
    return OBSERVORE_US_BASE_HZ + bin * OBSERVORE_US_STEP_HZ;
}

/* How far above its own learned floor a bin must sit to count as a tone.
 *
 * A ratio rather than an absolute level, because the floor is whatever this
 * room and this microphone produce and no fixed number describes both a
 * quiet flat and a train platform. Eight is about 9 dB. */
#define OBSERVORE_US_RATIO     8.0f

/* And an absolute floor, because a ratio alone cannot survive silence.
 *
 * The floor each bin is judged against is learned from the room. Start in a
 * room that is digitally silent -- or simply quieter than the arithmetic can
 * represent -- and the learned floor is zero, after which *any* signal is
 * infinitely above it and every bin reports a beacon at once. That is not
 * hypothetical: it is what the first version did the moment it was handed a
 * block of zeros.
 *
 * Measured with the bank as it now stands, per block of 960:
 *
 *   a 19 kHz tone at 0.1% of full scale   2.3e-4
 *   broadband noise at 2% of full scale   4.0e-4 in its loudest bin
 *   a loud 8 kHz tone, outside the band   4.4e-12
 *   numerical residue of true silence     < 1e-15
 *
 * 1e-6 sits four orders below the faintest tone worth reporting and nine
 * above the residue, so it rules out nothing real and rules out silence. */
#define OBSERVORE_US_MIN_POWER 1e-6f

/* And for how many consecutive blocks, which is what separates a beacon from
 * a jangle of keys. A tracking tone is emitted to be detected, so it lasts
 * long enough to be: schemes in the literature run tens of milliseconds to
 * seconds. Ten blocks is 213 ms. A key, a coin or a camera shutter is a
 * click -- broadband and over within one. */
#define OBSERVORE_US_RUNS      10

void observore_us_reset(observore_us_t *u);

/* Analyse one block of mono samples. Returns the bin of a tone that has just
 * reached OBSERVORE_US_RUNS consecutive blocks, or -1.
 *
 * Reports a bin once per episode: the run keeps counting while the tone is
 * there, but only the block that crosses the threshold answers, so a tone
 * held for a minute is one finding rather than three thousand. */
int observore_us_feed(observore_us_t *u, const int16_t *samples, size_t n);
