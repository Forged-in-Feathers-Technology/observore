# Verifying a XIAO ESP32-C5 or XIAO ESP32C6

Both board profiles are compile-tested only. Everything below was read off
Seeed's documentation rather than measured, so this is a list of things that
could be wrong, ordered by how likely they are to be wrong.

Flash from <https://observore.forgedinfeatherstechnology.com/> and pick the
board from the dropdown. That choice is the whole point, because the installer
can only tell an ESP32-C5 from an ESP32-S3, not a XIAO C5 from a Waveshare one.

## 1. LED polarity, most likely to be wrong

Assumed **active low**, meaning the pin is driven LOW to light it, from the
rest of the XIAO family. Never verified on either board.

- **Expected:** a short wink roughly every 5 seconds while idle, and solid on
  while the console is up.
- **If inverted:** the LED is on almost all the time and blinks *off* briefly.

That is the whole test. If it is lit constantly, polarity is wrong. It is a
one-line fix (`CONFIG_OBSERVORE_LED_ACTIVE_LOW`), so just say which way it went.

## 2. The C5's USB port, second most likely

The XIAO C5 profile assumes the USB-C socket is the chip's own USB-Serial/JTAG,
with no UART bridge. Third-party coverage says so; Seeed's own page does not
say it in as many words.

- **Expected:** boot messages appear on the serial console at 115200 after
  flashing, without touching anything.
- **If wrong:** flashing works but the log is silent.

A silent log with a device that is otherwise alive is the signature. (We hit
exactly this on the Waveshare C5, from the opposite direction.)

## 3. The button

- **C5:** BOOT on GPIO28. **C6:** BOOT on GPIO9.
- **Test:** hold it ~1.5 s. The log should say the mode swapped between patrol
  and uplink. Hold ~4 s for the console SoftAP.
- **If nothing happens**, the pin is wrong.

## 4. Console password

Printed at every boot, and nowhere else:

```
console SoftAP: "console-XXXXXX"  password: xxxxxxxxxxxx
```

Worth confirming it appears, since it is the only way in.

## 5. C6 only: do notifications work?

**This is the one we most expect to fail.** The XIAO C6 has no PSRAM, so
mbedTLS runs entirely in internal RAM. On a board that *does* have PSRAM it
took three separate changes to make a single small HTTPS POST succeed.

- Configure a notifier in the console, press **Test**.
- **If it fails**, please capture the serial log around the attempt, the
  `mbedtls_...` line and the `heap ... free, ... min, ... largest` line are the
  two that matter. Those two numbers told us everything last time.

Not a regression if it fails; it is the known limitation. Knowing *how* it
fails is what is useful.

## 6. Does it actually detect anything?

Let it run 10 minutes in a normal room.

- `scan: N APs` should show a plausible count. On a C5 it should include
  5 GHz, as in `scan: 28 APs (13 on 2.4 GHz, 15 on 5 GHz)`. **A C6 is 2.4 GHz only**,
  so no split is shown there and that is correct.
- The heartbeat line should show sightings climbing.
- Nothing should reboot. `rst:0x` appearing more than once means it did.

## What to send back

The first 30 seconds of boot log, one heartbeat line, and the answers to 1–3.
That covers everything unverified. Anything else is a bonus.
