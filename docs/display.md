# The display boards in detail

Touch calibration, the pocket-watch face, and brightness. The basics are in
[The screen](../README.md#the-screen).

## The pocket watch face

On the round board the first page is a watch. Hour and minute hands, the date
where a pocket watch keeps it, and a silver ring — and that is all it is until
you touch it. The seconds hand and the button bar appear for fifteen seconds
after a tap and then go away again, because a bar reading *page / baseline /
light* across a watch face gives the game away as surely as a warning banner
would.

The verdict is there, but only as a **mark**: the twelve hour markers are grey
while things are clear, amber at caution and red at alert. Nothing else on the
page changes, there is no text, and a glance from across a room reads as
somebody checking the time. A device whose screen announces that it has found
a body camera is a device that announces it to the person wearing one.

Minute ticks are deliberately absent — at this radius they turn into a grey
band — and the hour hand moves with the minutes, as a real one does, which is
720 positions round the dial rather than twelve. If the clock has never been
set the face says `not set` rather than drawing midnight, which is what an
unset clock would otherwise claim with total confidence.

**The dial walks, slowly, so the panel does not keep it.** An AMOLED ages
where it is lit, and this page is the one thing on the device that holds still
indefinitely: a bright ring and twelve markers in the same pixels for as long
as the device is on. So the whole face steps around a circle three pixels
across, eight positions, one step a minute, which means no pixel holds a
bright element for more than a few minutes at a time. The dial gives up three
pixels of radius to pay for the room, out of 229.

Three details make it invisible rather than merely present. The positions sit
on a circle rather than a raster, so consecutive steps are adjacent and the
dial never jumps across the face. The step comes from the clock rather than
from a counter, so it is continuous across a reboot — a device restarted every
morning would otherwise begin every day on the same eight pixels. And it is
held still while the face is awake: the walk is about two pixels and nobody
would call it wrong, but a dial that twitches while you are looking at it is a
thing you would notice, and burn-in accrues over the hours when nobody is.

The arithmetic for it lives in `observore_watch.c` beside the hands rather
than in the display, so the host tests can hold it to the rule that matters:
that the ring keeps its four-pixel margin at *every* step of the walk, not
just at the one it happened to be drawn at on the bench. Checking only that
the ring stays on the glass would have passed a dial that never paid for the
walk at all — it would still have fitted, by one pixel, sitting on the bezel.

## Brightness that follows the room

The Cheap Yellow Displays carry a photoresistor, and on the 3.5" board it is
GPIO34 — found by probing the pins the display and touch leave free and
watching which one moved when a hand covered the screen. With
`CONFIG_OBSERVORE_DISPLAY_LDR_GPIO` set, the backlight gains a fifth setting,
**Auto**, which is the default on a board that has the sensor: a bright panel
in a dark room announces the device, and asking a person to remember to dim it
defeats the point.

**The sensor learns its own range rather than being told one.** The first
version had fixed thresholds taken from a bare board where covering it reached
1,700 counts. On a board in a case they were useless: the room produced 1,157
to 1,298, the whole span below the second edge, so two of the four steps could
never be reached and the screen simply stayed bright. A case over the
photoresistor is enough to do that, and nobody should have to know it
happened. So the extremes seen are remembered and the steps divide whatever
range the board actually experiences — measured afterwards on that same board:
657 to 997, using all four steps, and in the opposite direction from the bare
board, which the adaptive version does not care about either.

It holds still until it has seen at least 120 counts of variation, because in a
room of unchanging light every flicker would otherwise swing the backlight.
The level follows twice a second, with hysteresis at eight per cent of the
learned span, since a reading sitting on a boundary makes the panel pulse every
time somebody passes a lamp.

It cannot do better than that. **The board has no way to tell whether it is on
USB or the cell**, so "dim on battery, bright when plugged in" is not
available on this hardware — the same finding as the missing battery
percentage. Ambient light is a fair proxy, since the dark room where the screen
should be dim is usually also where the battery matters.

## Brightness without touch

The backlight level is also on the console, under **Screen**, for a board that
has a panel but no touch — otherwise there would be no way to dim it. The
panel is hidden entirely where there is no screen rather than shown as a
control that refuses.

**Wi-Fi buffers belong in PSRAM where there is any.** The round board's
internal heap floor sat at about **three kilobytes**, reached during patrol
and landing on nearly the same number across runs hours apart — the signature
of a large allocation that only just fits, rather than of a leak. Patrol is
when the sniffer is switched on and off every cycle and the driver takes its
thirty-two dynamic receive buffers, out of internal memory, on a board with
eight megabytes of PSRAM holding nothing but a watch face.

Moving them raised the floor to **eighteen kilobytes**, six times the
headroom, with no measurable cost to sniffing: 99.7% of frames still
captured. It also made the largest free internal block *smaller*, which
broke a different thing — the fifteen-kilobyte DMA buffer the panel clear
allocated per call no longer fit. That buffer is allocated once at startup
and shared now, which is better anyway: a block taken and released
repeatedly is a block that fragments the heap it lives in.

**On the 3.5" board's battery connector.** It charges a cell and reports
nothing about it — with one failure that looks exactly like the connector not
working at all.

A lithium pack with a protection board that has been taken below roughly 2.5 V
**latches off**. Its terminals then read zero, the board's charger sees nothing
to charge and never starts, and the symptom is a device that goes dark the
moment USB is unplugged. That is indistinguishable, at the connector, from a
board with no charge circuit fitted — which is a real possibility on other
boards in this family and sent one afternoon looking in the wrong place.

The recovery is an external charger that will push a latched pack, or simply a
known-good source; the board cannot do it. Worth checking polarity with a
meter first regardless, because this family is reported to wire its JST
connector against the usual convention, and reverse-feeding a lithium cell is
a fire risk rather than an inconvenience. On the bench board here the polarity
was correct.

**Charging while it runs needs more current than a computer's USB port gives.**
With a charged cell on the connector and the board plugged into a laptop port,
it boot-loops continuously. On a proper USB supply with the same cell attached
it runs fine, and either source on its own has always been fine. So the rule is
about the *supply*, not about the combination: charge it from a wall adapter, or
charge the cell off the board.

The mechanism, because the symptom is misleading enough to be worth writing
down. Each cycle reaches exactly the same point and stops:

```
I (685) observore: Observore ... starting -- board cyd-3248s035r-st7796
```

Always 685 ms, across eighty-nine consecutive cycles. That is not noise; it is
a deterministic trigger, and the step immediately after that line is
`observore_wifi_init()` — the radio powering up, which is the largest current
step in startup. Add the charger drawing from the same VBUS and the port cannot
hold the rail.

The confirming detail is that the **USB-serial bridge resets too**: the device
node re-enumerated eighty-nine times, which an ESP32-only fault cannot do. It
is VBUS collapsing, not the 3.3 V regulator, and not firmware. The intermittency
fits as well — charge current is highest into a cell that has been sitting and
tapers as it fills, so the margin moves while you watch.

One practical trade: a wall adapter fixes it and takes the serial console with
it. For bench work, one supply at a time remains the simpler habit. The three ADC-capable pins the display and touch leave free
were probed on a board running from a battery: 35 and 39 read zero, and 34
swung from 1,101 to 1,734 counts when a hand covered the screen — that is the
ambient light sensor, not a supply. A percentage would need a divider soldered
from the cell to a free pin. The run history answers the question that matters
without any of that: the figure above, 25.1 hours, was measured rather than
estimated.

That board does not send notifications. It cannot: the notifier is a build
option (`CONFIG_OBSERVORE_NOTIFIER`) and the CYD profile leaves it out, which
is what makes the port fit. Every memory problem this project has had was on
the uplink and most of it was the TLS handshake behind a notification, so the
board with the least RAM and no PSRAM is better off without the code than with
it switched off. Off means absent: the console hides the panel, and asking to
configure a provider says "this build has no notifier". Any board can be built
that way; the CYD is the one that is.

The profiles are named for the panel controller, `cyd-2432s028r-st7789` and
`cyd-2432s028r-ili9341`, not just the product, because the same product ships
with either, a build for one shows garbage on the other, and the chip is
identical so the flasher cannot tell them apart. If you do not know which you
have, flash one: the wrong one is a negative or has red and blue swapped, and
the right one is the other build. The ST7789 profile was settled on the bench;
the ILI9341 one carries the settings every other project uses for that
controller and is waiting for someone with the board to confirm them (an
issue with a photo is all it takes). Three things about the ST7789 panel
were settled on the bench against what is written about it: it does not want
colour inversion, the SPI path does not byte-swap for you, and the panel driver
returns before DMA has read the buffer you handed it. Each is a build setting
or a comment where the next revision will look.

## Calibrating a panel

A resistive sheet does not read the same from one assembly to the next, so the
bounds compiled into a profile are one unit's measurements — ours. If presses
land a row or two off where you aimed, the panel in your hand simply reads
differently, and the fix is on the device rather than in a rebuild.

**`calibrate`** is the fourth action on the system page, on the boards that
have a resistive panel. It shows a target near one corner and then near the
other, and stores what it measured in NVS, where it survives an update like
the Wi-Fi details do. The button reads `recalibrate` afterwards. It reads the
**raw** channels rather than the mapped ones, because the mapping is the thing
being corrected.

Press the middle of each cross rather than the corner itself. The bezel
overlaps the glass on these boards and a press right at the edge often does
not register at all — which is how the 3.5" panel's first calibration came
out short and squashed its bottom three rows together. That is also why the
targets sit two characters in from the edge, and why the arithmetic scales the
measured span back out to the full sheet instead of assuming the press landed
where the cross was drawn. A stored calibration with no extent is ignored
rather than divided by.

A capacitive panel has nothing to calibrate: the FT3168 reports panel pixels
directly, so the round board has no such action and never needed one.

**A calibration saved before v0.13.0 is discarded, and has to be done again.**
Until then the arithmetic paired each touch channel with the wrong screen axis,
so every calibration written by an older build is wrong -- and keeping one
across an upgrade would look exactly like the bug never being fixed, because
the symptom is identical. The device says so on the serial log and falls back
to the compiled bounds, which are merely somebody else's measurements rather
than impossible ones.

That check is on *provenance*, not on the numbers, and the difference is worth
recording because the first attempt got it wrong. Recognising a bad calibration
by its values looks easy -- a span wider than the converter can read, an
endpoint below zero -- and does not work. Extrapolating from inset targets
assumes the sheet is linear, and near the bezel it is not quite, so a correct
calibration can overshoot both ends by a few counts and exceed the range too.
One board's bad bounds were 136 counts over, well inside any tolerance wide
enough to admit a real one. And on a panel whose usable range is narrower, the
same bug produces bounds entirely inside the valid range and is undetectable by
value at all. So the stored calibration now records which arithmetic made it,
and anything that does not say is not trusted.

**Porting a new board** is the case calibration does not cover, because two
of the three variables are structural rather than per-unit: whether the touch
axes are crossed relative to the landscape display, and which way each one
runs. Build with `CONFIG_OBSERVORE_TOUCH_LOG_RAW=y`, press the screen, and
read the values off the log.

Check the crossing first, and check it with a press to the left and then a
press to the right **at the same height**. On the panels here that single move
swings raw Y across its whole range while raw X barely stirs, which is what
`CONFIG_OBSERVORE_TOUCH_SWAP_XY` exists for. Corner presses will not tell you
this: a corner moves both axes at once, and two corners that share an edge
look exactly like a dead axis — which is what they were read as here, through
a board swap and a hunt for a fault that did not exist, before one deliberate
left-right press settled it in ten seconds.

Get those two right and the bounds no longer have to be: hand the board to
whoever owns it and let them press two crosses.
