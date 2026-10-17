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
