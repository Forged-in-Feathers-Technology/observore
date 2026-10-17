#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The microphone, used only to look for ultrasonic tracking beacons.
 *
 * See observore_ultrasound.h for what that means and why it belongs on a
 * counter-surveillance device. This file is the hardware half: an I2S MEMS
 * microphone sampled at 48 kHz, one 20 ms block at a time, each block handed
 * to the analyser and then overwritten.
 *
 * ## The promise this module has to keep
 *
 * Nothing is recorded. No audio is buffered beyond the single block being
 * analysed, none is written to flash, and none is sent anywhere. The only
 * things that outlive a block are twenty band energies and a counter.
 *
 * That is not a comment, it is the design: there is no buffer to grow, no
 * file to open and no API that returns a sample. A detector that keeps a
 * microphone open is a listening device, and "it means well" is not
 * something the person holding it can check. What they can check is that
 * the code has nowhere to put the audio.
 *
 * Off unless a board profile turns it on, and gated at runtime on top of
 * that, for the same reason.
 */

void observore_mic_init(void);

/* Whether the microphone is listening. Persisted; starts off. */
bool observore_mic_enabled(void);
void observore_mic_set_enabled(bool on);

/* The last beacon seen: its frequency in Hz and how long ago, or false if
 * there has not been one since boot. */
bool observore_mic_last(int *hz, int64_t *seconds_ago);

/* How many distinct beacon episodes since boot. */
uint32_t observore_mic_count(void);

/* The band energies from the most recent block, and the floors they are
 * judged against. Copies at most `n` of each; returns how many bands there
 * are.
 *
 * Here because "it did not detect anything" has several very different
 * causes -- a microphone that is not wired, a microphone that works but
 * hears nothing above the band, a tone that is present but too faint to
 * clear the threshold -- and they are indistinguishable from the outside.
 * The numbers tell them apart: all zeros is a dead capture, a sensible floor
 * with nothing above it is a quiet room, and a raised bin that never reaches
 * the ratio is a source too weak to matter. */
int observore_mic_bands(float *power, float *floor_, int n);

/* Prove the whole chain against a real sound in real air.
 *
 * The speaker and the microphone are centimetres apart on this board, so it
 * can test itself: emit a tone in the ultrasonic band, and see whether the
 * band it came out of is the band that lights up. That exercises everything
 * a real beacon would -- I2S out, amplifier, speaker, air, microphone, I2S
 * in, the filter bank -- where a synthetic buffer exercises only the last of
 * those, and the host suite already does that.
 *
 * It sweeps rather than playing one tone. A small 8-ohm driver's response at
 * 19 kHz is poor and unknown, so a single frequency that fails to carry
 * would look like a broken detector; a sweep says which part of the band
 * this speaker can actually reach, and the per-band numbers say how well.
 *
 * Mostly above hearing, but do not promise silence. The tones themselves
 * are above the range most adults hear, and the lowest are not above
 * everyone's -- hearing varies far more between people than the round
 * numbers suggest. The first version of this also made its loud tones
 * start and stop instantly, and a step is broadband: it was reported from
 * the room as a low hum while the log insisted it was inaudible. The tones
 * were; the edges were not, and they are ramped now.
 *
 * It runs regardless of whether sound is switched on: refusing to let
 * somebody check that a privacy feature works unless they had first enabled
 * alerts would be a poor trade.
 *
 * Blocking, a few seconds. Returns how many of the tried frequencies were
 * heard back in the right band.
 */
int observore_mic_selftest(void);

/* The loudest thing the last block contained, across the whole band, as a
 * rough level. Zero when nothing has been captured. */
float observore_mic_peak(void);
