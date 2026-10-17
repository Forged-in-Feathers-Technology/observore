#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "observore_track.h"

/* Sound, on the boards that have a speaker.
 *
 * Off by default, and deliberately so. This device's whole posture is to not
 * announce itself -- the watch face exists because a thing glanced at in a
 * pocket is far less conspicuous than one showing a list of everybody's
 * Bluetooth addresses -- and a chirp when a threat is spotted tells the room
 * you spotted them. That is the opposite of the point.
 *
 * It is still worth having, because the posture is not universal: a detector
 * left watching an empty flat has nobody to hide from, and a sound carries
 * from the next room where a screen does not. So the hardware is a build
 * option, whether it ever sounds is a runtime setting, and both start off.
 *
 * The 1.85" round LCD is the only board here with the parts: a PCM5101 I2S
 * DAC into a power amplifier into an 8-ohm speaker on a header. The DAC has
 * no I2C presence, so unlike every other chip on that board it cannot be
 * probed -- the boot scan will never show it, and the only way to know it is
 * wired is to drive it and listen. Hence observore_audio_selftest().
 */

void observore_audio_init(void);

/* Whether sound is allowed at all. Persisted, and off until somebody says
 * otherwise. */
bool observore_audio_enabled(void);
void observore_audio_set_enabled(bool on);

/* Volume, 1 to 100, persisted with the on/off flag.
 *
 * 100 is not loud: the ceiling is a twelfth of full scale, because what this
 * drives is a 2-watt amplifier and the first version of this file set a fifth
 * of full scale thinking that modest. It was loud enough to be unplugged.
 * Pass -1 to leave the volume alone. */
int  observore_audio_volume(void);
void observore_audio_set(bool on, int volume);

/* The threat level changed. Rising levels get a sound, falling ones do not:
 * a device that chirps on the way back down is a device that chirps twice as
 * often for no added information. Does nothing when sound is off. */
void observore_audio_level(observore_level_t from, observore_level_t to);

/* One tone, blocking, for bring-up. Bypasses the enabled flag, because its
 * whole job is to answer "is the speaker plugged in". */
void observore_audio_selftest(void);

/* Emit one tone of `hz` for `ms`, bypassing the enabled flag.
 *
 * For the ultrasonic self-test: the speaker and the microphone are
 * centimetres apart on this board, so the board can prove its own beacon
 * detector against a real sound in real air rather than against a synthetic
 * buffer. Above about 17 kHz it is beyond most people's hearing, which is
 * why it is allowed to ignore the sound setting -- though "most" is doing
 * real work in that sentence, and refusing to run it unless the owner had
 * first turned alerts on would be refusing to let them check that a privacy
 * feature works. */
void observore_audio_emit(int hz, int ms);

/* A continuous tone, played in short chunks so the caller can do something
 * between them -- specifically, listen.
 *
 * observore_audio_emit() blocks until the DMA ring has swallowed the whole
 * tone, which means that by the time it returns the tone has essentially
 * finished. A self-test that called it and then started reading the
 * microphone was listening to the silence afterwards, and reported that the
 * speaker could not reach the band when the real fault was that nobody was
 * playing anything by the time anybody listened.
 *
 * `start` sets the frequency and `chunk` queues roughly `ms` more of it;
 * interleave chunk with a read and the tone is still sounding when the
 * block is captured. `stop` releases it.
 *
 * `peak` overrides the volume setting, 0 to use it. That override exists for
 * the ultrasonic self-test and is deliberately allowed past the normal
 * ceiling, because the ceiling is there to protect ears and a tone nobody
 * can hear does not threaten any. A small 8-ohm driver's response at 19 kHz
 * is tens of decibels down on its response at 1 kHz, so reaching the
 * microphone at all takes a drive level that would be unbearable if it were
 * audible -- and is not, because it is not. */
void observore_audio_tone_start(int hz, int peak);
void observore_audio_tone_chunk(int ms);
void observore_audio_tone_stop(void);
