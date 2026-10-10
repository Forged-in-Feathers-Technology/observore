# Observore

[![CI](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml/badge.svg)](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Forged-in-Feathers-Technology/observore?sort=semver)](https://github.com/Forged-in-Feathers-Technology/observore/releases)

A passive counter-surveillance detector for nine ESP32 boards, in two
families: headless sensors — the [Seeed Studio XIAO
ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/), the
dual-band [ESP32-C5](https://www.espressif.com/en/products/socs/esp32-c5) and
the C6 — and the **"Cheap Yellow Display"** boards, which have a screen and a
touch panel. Built by [Forged in Feathers
Technology](https://www.forgedinfeatherstechnology.com).

It tells you what is watching you. Built to sit in one place and watch that
place: it learns what is normally there, then reports what is new. It listens
for the radio signatures of body cameras, licence-plate readers, IP cameras,
Bluetooth trackers, smart glasses and Remote ID drones — and for the equipment
that transmits rather than watches: a Flipper, a pwnagotchi, a Pineapple, a
flood of deauthentication frames knocking devices off a network. It scores what
it finds and either pushes notifications and serves the log on your network, or
shows it on its own screen, depending on the board.

Observer and omnivore: it eats surveillance signals.

### Which board

The choice is not about speed or memory. It is whether the device reports to
you over the network or to your hand.

A **headless board** — the S3, either C5, the C6 — pushes notifications to
Gotify, ntfy, Pushover, a webhook or Telegram, and serves its log on your
network. It is the one to leave somewhere and read from elsewhere.

A **Cheap Yellow Display** shows what it sees on its own screen, and with touch
you can set a baseline, dismiss a finding, join a network by typing the password
on the glass, and read the console's address without a serial cable. It sends no
notifications at all — the notifier is left out of the build, which is what
makes it fit on a chip with no PSRAM, and every memory problem this project has
had was the TLS handshake behind a notification. A screen is a personal display,
so the trade is a fair one.

Five of the nine have screens: the 2.8" board in its two panel revisions,
the 3.5", the C5 one below, and the round one after it. See [the screen](#the-screen) for what
they can do and [boards, which are not the same as
chips](#boards-which-are-not-the-same-as-chips) for the profiles.

RockBase's **NM-CYD-C5** is a Cheap Yellow Display with an **ESP32-C5**
behind it, and it is the first board here that both sees 5 GHz and has a
screen to say so on — every other display board is a plain ESP32 that cannot sweep the
upper band at all, and every board that could was headless.

The same 2.8" ST7789 panel as the classic CYD and almost none of the same
pins, taken from RockBase's own TFT_eSPI setup rather than from a
description, which gave the display chip select as GPIO 3 where it is 23.
Two things about it are genuinely different: the touch controller's
interrupt line is not wired anywhere, so the XPT2046 driver gained a polled
mode — the interrupt was only ever an optimisation, and the pressure
threshold was always what decided — and the panel takes its pixels in the
opposite byte order to the other ST7789 boards here, which took two readings
to establish because each setting alone produces a plausible wrongness.

What held it back was neither of those. Its touch bounds are the compiled
defaults, because nobody has measured this panel — and the numbers in every
other profile are one bench unit's, which is a thinner claim than it looks.
A resistive sheet varies between assemblies of the same product, so shipping
a profile meant shipping somebody else's measurements and hoping. The
**calibrate** action settles it on the device instead: two presses and the
panel in the owner's hand teaches it its own bounds. See [calibrating a
panel](#calibrating-a-panel).

The round one is different enough to describe separately. Waveshare's
**ESP32-S3-Touch-AMOLED-1.43** is a round 1.43" AMOLED on an S3 with 16 MB of
flash and 8 MB of octal PSRAM — capacitive touch rather than resistive, a
hardware clock, and an **accelerometer**, which no other board here has. That
last one is what makes the `tailing` class possible: it is the only board
that can tell being carried from sitting still.

One product ships with either of two panel controllers, so the firmware asks
the panel which it is at boot rather than being built for one. That is what
made it safe to offer for download — a stranger cannot know which revision
arrived in the post, and the failure, a blank screen or an image six pixels
sideways, would tell them nothing.

**Brightness is a command rather than a pin** on this panel, and over four
data lines a command does not travel as itself: with no D/C line the opcode
goes in the address phase, so a write is `0x02`, the register, a pad byte,
packed into one 32-bit word. Sending the bare register is ignored silently —
which is what shipped at first, so the brightness button cycled four
settings, saved each to NVS, redrew the screen and changed nothing. The panel
sat at full brightness through every soak run on this board.

**Capacitive touch** removes most of what the resistive path has to do, and
adds one requirement instead: a bare finger, or a capacitive stylus. A
passive plastic one registers as nothing at all, correctly and permanently,
and the device cannot tell that apart from nobody touching it. The
FT3168 reports how many fingers are down and where each one is, already in
panel pixels, so there is no pressure threshold, no median filter and no
calibration: the two days of arithmetic and one wrongly accused ribbon cable
that the Cheap Yellow Displays cost simply do not happen. It shares its two
wires with the motion sensor and the clock, and the driver says which of the
three answered at startup, because "the panel is not answering" and "the
panel is not there" look identical in a log that mentions neither.

**A round screen** is the first one here that is not a rectangle, and the text
grid handles it by not trying to be clever. The largest square inside a circle
of 466 pixels is 329 across, which is 41 columns by 20 rows — slightly more
text than the 2.8" boards show, in a smaller space — and the four corners are
simply never drawn into. Touches outside that square are *clamped* into it
rather than discarded: the crescents are live glass, not bezel, and the
seventy pixels below the button bar are where a finger aiming at the bar
actually lands. Text does not reflow around a curve legibly at eight
pixels a character, and a page of readings clipped at the corners is worse
than one that is smaller and whole.

## Getting started

You need one of the nine supported boards and a USB cable. The whole first run
takes about ten minutes, most of it waiting.

The browser flasher detects the chip and offers the builds that fit it. There is
usually one, and sometimes a choice: three of the boards are ESP32-C5s whose
images are not interchangeable, and the 2.8" and 3.5" displays are both plain
ESP32s with different panels. The flasher says which is which, and the wrong
choice is recoverable — a mismatched display build shows a blank or garbled
screen and is fixed by flashing the other one.

The console page is served gzipped, so a command-line client needs
`curl --compressed` (a browser needs nothing).

**The quickest route is the [browser
flasher](https://observore.forgedinfeatherstechnology.com/)** — any
Chromium-based desktop browser (Chrome, Edge, Opera, or Brave 1.69 and later),
no toolchain. Firefox and Safari do not implement Web Serial, and no mobile
browser does. It writes the firmware without touching the
stored configuration, so it upgrades an existing device without losing its mute
rules, credentials or console password. Skip to step 2 below once it finishes.

To build it yourself instead you also need
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5.5**, which is
what CI is pinned to.

Not "v5.5 or later", and the difference is not cosmetic. Images built on the
v5.5 line run perfectly when flashed over USB and hang during early startup
when booted from the second OTA slot, somewhere between the PSRAM memory test
and the first line of application code. Every over-the-air update rolled back
and none ever took. The same source built on v5.5.5 boots from that slot and
confirms itself. What actually differs inside ESP-IDF was never identified,
only that it does, which is why the pin is exact rather than a floor.

**1. Build and flash.**

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The XIAO uses the S3's native USB-Serial/JTAG, so it appears as
`/dev/ttyACM0`, not `/dev/ttyUSB0`.

**2. Write down the console password.** It is generated once, on this device,
and printed at every boot:

```
console SoftAP: "console-XXXXXX"  password: xxxxxxxxxxxx
```

You will need it in a moment, and serial is the only place it appears. See
[The console password](#the-console-password).

**3. Open the console.** Hold the BOOT button for 1.5 s. The LED goes solid and
the log says the SoftAP is up. Join `console-XXXXXX` with the password from
step 2 and open <http://192.168.4.1/>.

If nothing happens, keep holding: the firmware prints how long it saw the
button held, so a press slightly too short looks different from a dead button.

**4. Give it your Wi-Fi.** In the **Network** panel enter your SSID and
password, and Save. Then hold the button for 1.5 s again — it joins, and the
log prints the address the console now lives at:

```
uplink up at 192.168.1.42
```

From here the console is on your LAN and the SoftAP is no longer needed. Note
that it is only on your network during its uplink window, so the address
answers for about thirty seconds out of every two and a half minutes. That is
[deliberate](#patrol-and-uplink-alternate).

**5. Optionally, set up notifications.** In the **Notifications** panel, put in
a [Gotify](https://gotify.net/) server URL and application token, Save, then
**Send test** while the uplink is up.

**6. Leave it where it will live, and take a baseline.** Let it patrol for ten
minutes or so, then open the console and press **Set baseline**. That marks
everything currently in range as known and resets the score, so from then on it
reports what is *new* rather than the whole neighbourhood.

**Then expect quiet.** After a good baseline Observore should say almost
nothing. A silent device is the normal state, not a broken one. The heartbeat
on the serial log tells you it is still watching.

### Warning the neighbours

Several nodes see more than one does, and a second vantage point is the
discriminator this project keeps failing to find on its own — three revisions
of the `tailing` class each reached for a property of the *suspect* when the
useful question was always about the surroundings
([#132](https://github.com/Forged-in-Feathers-Technology/observore/issues/132)).
The listening half shipped first: a node hears its neighbours' warnings and
costs nothing to do it, because the radio is already scanning.

`CONFIG_OBSERVORE_MESH_TX` is the other half, and it is **off by default**.

**What it spends is the property this device is built on.** Everything else
here is receive-only. A node that warns can be found by direction-finding, and
"there is an observer here" is the one thing a counter-surveillance device
should not announce. There is an irony to pay for as well: this project
classifies SquachWatch as a `peer-detector` precisely *because* it announces
itself in mesh mode, so a warning Observore becomes a peer-detector in
somebody else's device — including another Observore.

**Off means the capability is absent, not disabled.** The option selects
NimBLE's broadcaster role, so without it the code that advertises is not
compiled and there is no runtime path to an emission. That is worth more than
a boolean somebody could flip, and it is the same construction that keeps
automatic suppression out of the monitors: by not having the caller.

**A node with nothing to warn about stays silent.** It transmits when it has a
finding worth passing on and not otherwise, so an idle node is exactly as
quiet as a build without this.

What it will not warn about, and why each one matters:

- **anything with its monitor off** — the owner said stop reporting this, and
  a warning is a report that leaves the box
- **anything the census quieted** — household furniture is household *here*,
  and broadcasting it asks the neighbours to carry a judgement about a room
  they cannot see
- **anything worth no points** — a peer detector or a fixture is a fact about
  the room rather than a threat in it, and the cheapest attack on a mesh is to
  fill it with true but useless statements

One finding per burst, the heaviest currently in front of the radio. The
findings list is already filtered by the monitors and the census, so the thing
warned about is exactly the thing this node would report to its owner: it
never tells a neighbour something it would not tell the person holding it.

**The address is random and regenerated every burst.** The payload's node id
is sixteen bits and deliberately coarse; leaving the device's own BLE address
on the air would undo that, because an address that does not change says "this
same box was here yesterday" to anyone keeping a list. A fresh non-resolvable
address says only "an Observore is near", which is the claim being made on
purpose.

**The interval is randomised**, 90 to 150 seconds. A fixed cadence is itself a
fingerprint: something listening for a beacon every ninety seconds exactly has
an easier job than something listening for one in a window.

**It says so while it is doing it**, in the three places that cannot be
missed: the coloured band on the glass carries a hexagon *and the peer count*,
the system page says `mesh  warning, N peers in range` or `listen only, never
transmits`, and `/api/status` reports `peers: {nodes, warnings, tx}`. The
count matters more than the state — `⬡0` means the exposure is being paid for
and nothing is coming back, and an on/off mark hides exactly that case.

The hexagon is real work rather than a character. `tools/gen_font.py` renders
DejaVuSansMono into an 8×16 cell and covered printable ASCII only, so there
was no hexagon to print and the band said `TX` instead. Icons now follow
ASCII in the table, rendered from the *proportional* face — because the
monospace one has no hexagon and draws U+2B21 as its `.notdef` rectangle.

That nearly shipped as a rectangle labelled "mesh". Pillow reports a mask size
of (8, 12) for the missing glyph, which looks like a glyph until you print the
pixels. Three things came out of it:

- **The generator refuses to emit a notdef box.** It renders U+FFFF — a
  noncharacter no font has — and any icon whose bitmap matches it stops the
  build.
- **The header shows the shape.** Each icon is emitted with an ASCII-art
  drawing above it, so the glyph can be reviewed without a screen, which is
  the only way the rectangle was caught in the first place.
- **A host test refuses the shape of that failure** without pinning the exact
  bitmap: a rectangle's top and bottom rows are the same long run, and a real
  icon tapers. Re-rendering at another size should not fail a test; drawing a
  box should.

Raising the table's last code to 0x7F also made the compiler find a latent
bug: `char` is signed on both toolchains, so `ch > OBSERVORE_FONT_LAST` became
always-false and `-Werror=type-limits` refused it. Both glyph routines — the
panel's and the round watch face's — took a signed `char`, so the *second*
icon added would have arrived as a negative number and drawn a question mark.
Both take a `uint8_t` now. #132 said the mesh mark would not be the last icon
this project wants; adding one is a line in `ICONS` and sixteen bytes.

No position is sent. The format carries one and the decoder reads one, but
this device has no fix to put there; a GPS node is where that field starts
being filled, and the encoder refuses zeroes rather than claiming the Gulf of
Guinea.

## What it does

**While patrolling, it listens and does not answer.** Nothing it observes can
observe it back, because nothing leaves the radio:

| | |
|---|---|
| BLE scan | passive — no `SCAN_REQ` is ever emitted |
| Wi-Fi access-point scan | **passive**, no probe requests |
| Wi-Fi sniff | receive-only |
| BLE warning bursts | **off unless built in** — see below |

That last row is the one exception, and it is off in every default build. See
[Warning the neighbours](#warning-the-neighbours) for what it costs and how a
build without it cannot do it at all.

The Wi-Fi scan being passive matters as much as the BLE one. An active scan
broadcasts probe requests carrying the device's own MAC on every channel, every
cycle, and probe requests are exactly what presence analytics and Wi-Fi
tracking systems collect. A detector that announced itself to the things it
was built to notice would be self-defeating. The cost is dwell time, not
coverage: access points beacon around ten times a second, hidden ones included.

It does transmit in the other two modes, and there is no way around that:

- **Console:** the SoftAP beacons and serves the page.
- **Uplink:** it is associated to your network and pushes notifications.

Both are entered deliberately, and patrol is where it spends most of its
time.

Detection runs across three phases:

| Phase | What it catches |
|---|---|
| BLE passive scan (continuous) | trackers, body cameras, smart glasses, Remote ID drones, followers |
| Wi-Fi passive scan (~10 s/cycle) | camera and ALPR vendor APs, camera-keyword SSIDs |
| Wi-Fi promiscuous sniff (~5 s/cycle, ch 1–13) | Remote ID beacons, hidden and non-broadcasting APs |

Classification uses four independent kinds of evidence, and the UI tells you
which one fired so you can judge a hit rather than just trust it:

- **OUI** — 209 vendor prefixes generated from the IEEE MA-L registry
  (bodycam, ALPR, camera, fleet-telematics, drone).
- **Payload signatures** — Apple Find My (`0x004C`/type `0x12`), Samsung,
  Tile (`0xFEED`), Galaxy SmartTag (`0xFD5A`), Google Fast Pair (`0xFE2C`),
  ASTM F3411 Remote ID over BLE (`0xFFFA`/`0x0D`) and over Wi-Fi
  (vendor IE `FA:0B:BC`/`0x0D`), SquachMesh (`0xFFFF` + `SQM1`), and Flipper
  Zero (company `0x0E29`, service UUIDs `0x3081`–`0x3083`).
- **Behaviour** — what something did rather than what it claims to be: a flood
  of deauthentication frames, or a pwnagotchi's own beacon.
- **Name and SSID keywords:** for hardware that announces itself.
- **Vendor labelling** — 10,348 benign vendor prefixes across 43 common
  manufacturers (Apple, Samsung, Ubiquiti, Espressif, Google, …), also
  generated from the IEEE registry. These **never classify and never score**.
  They exist so an anonymous MAC reads as "Apple" and you can recognise your
  own gear at a glance. Kept in a table entirely separate from the threat
  prefixes, because mixing "this is a surveillance camera" with "this is a
  Samsung" is how a detector starts crying wolf at its owner's phone.
- **Persistence:** the follower heuristic: an unclassified BLE address seen
  3+ times spanning 5+ minutes is reported as following you. This is the part
  that catches hardware with no signature at all, and it is the reason to
  build the thing.

### What a device says about itself

A vendor prefix is an inference: this address block belongs to that company, so
the device is probably theirs. It fails in two common ways. Modern phones and
most trackers randomise their address, leaving no prefix to look up at all, and
the IEEE registry does not cover every assignment.

Access points supporting Wi-Fi Protected Setup put an element in **every
beacon** naming their manufacturer, model and device name in plain text. That
is not an inference, it is the device stating what it is, and it works when the
prefix is unregistered, unknown, or absent. Cameras are the class this helps
most, which is also the class scored lowest on vendor evidence alone.

So a WPS manufacturer or model is matched against the same keyword table as an
SSID, and unlike an SSID it is allowed to override the vendor prefix. An SSID
is free text somebody chose; a WPS element is firmware describing its own
hardware.

How much this helps depends on what is around you, and it can be nothing at
all. Counting the vendor elements in 150 beacons from about 30 access points in
one flat found 450 of them and not a single WPS element among them. Ubiquiti
access points do not advertise it, and neither did anything else in range.
Where the element is absent the vendor prefix is all there is, so treat a
manufacturer string as a bonus rather than something to rely on.

The fields arrive in a frame from a device under nobody's control, so they are
length checked, bounds checked and stripped to printable characters before
reaching a log line, a JSON response or the console. The parser is
ESP-IDF-free and unit tested against truncated elements, attributes claiming to
be longer than the frame that holds them, and control characters aimed at the
console.

### Scoring

Each device present counts its class (bodycam, ALPR and deauth-flood 6,
tracker/drone/glasses/hunter 3, telematics 2, camera 1, accessory 1,
peer-detector and fixture 0, follower capped — see below), and the score is
their sum.

Six is one alert by itself, and that is the whole of the split. **Six names an
act**: a body camera is recording, a plate reader is reading plates, a flood of
deauthentication frames is knocking devices off a network. **Three names a
capability**: a Flipper, a drone, a tracker are all things that *could* be
aimed at you and usually are not, so they read caution alone and alert in
company.

**The score is what is here now, not a history of it.**

It used to accumulate: every device added its points again every two minutes,
against a decay of one point a minute in total. Anything worth two points or
more therefore outran the decay on its own, so the score climbed to its ceiling
and stayed there. Ninety-nine meant "something persistent has been here a
while" — never "how much is here" or "how serious". A board on a desk with a
Flipper and a few phones sat at 99 and alert indefinitely, and a level that is
always on is not read.

So the score is a sum over the devices currently tracked, worked out when
asked. It rises when something arrives and falls when it leaves; the device
table's own thirty-minute expiry is what brings it down, and is slow enough
that nothing flickers. There is no accumulator, no decay and no per-device
cooldown to tune — one device present fifty times is still one device.

**Followers cannot move the verdict at all.** Together they contribute at
most two — the top of clear — so the device lists every one of them and still
says "nothing here is identified as surveillance", which are two true
statements rather than one hedge. The verdict is reserved for things
identified as what they are.

Two is also less than the gap between caution and alert, which buys an
invariant worth stating plainly: **no quantity of unidentified devices can
push an identified one over the line.** A Flipper in an empty room and a
Flipper in a crowded bar both read caution, because the crowd is not evidence
about the Flipper.

That ceiling took four attempts, and the first three were wrong about what a
follower means. **Duration is not evidence** — everybody in a restaurant has
been near you for an hour. **Rotation is not evidence either**, which took
hardware to see: every modern phone changes its Bluetooth address every
quarter of an hour, so "survived a rotation" describes a phone behaving
normally, not a device evading notice. A house full of them put a board at
seventeen and alert. **Capping the class at the top of caution** fixed the
number and left the verdict wrong in a quieter way: two boards soaked
overnight in an ordinary room sat at exactly the cap for eight hours, amber
throughout, with nothing identified on either. A warning that is always on is
not a warning.

What remains true is that a follower is, by definition, **unidentified**. That
is worth listing and worth a glance. It is not worth an alarm, because the
device cannot say what the thing is. Within the cap the ordering still holds —
mere presence counts one, a rotation counts two — so the list still says which
of them has been followed across a rotation, while the ceiling keeps the class
inside clear.

Sustained presence is not lost; it has moved to where it belongs. A device does
not *become* a follower until it has been there five minutes. Duration decides
what something is, and the score says what is here.

### The one claim persistence cannot make

**`tailing`** is a follower that was beside you **while you were somewhere
else** — not one that was present before a journey and present after it,
which was the first attempt and which describes an entire household. A round
trip comes home, and everything indoors is in range at both ends: one walk to
a garden promoted twelve devices that had been sitting in the house for nine
hours.

So the evidence is presence **at the far end**, and the timing is the whole
of it. The window opens when the access points confirm the board is somewhere
else and closes half a minute later; only sightings inside it count.

Opening it when the board was picked up was not good enough, which took a
third walk to learn: that window starts at the front door, surrounded by
everything that lives in the house, and the strongest reading in it is taken
on the way out. A kitchen device measured at full strength while its owner
was still in the kitchen looked as though it had never faded. Two of three
promotions on a garden walk were household devices for exactly that reason. Something carried along holds its signal.
Something left behind either goes silent or arrives twenty decibels down and
gets loud again only when you walk back through the door — which is the same
fade test the access points use, pointed at the suspects instead of the
surroundings.

Not fading is necessary and is not sufficient, which took a second walk to
learn: **a device that is faint everywhere has nothing to fade.** A
neighbour's phone heard at −72 dBm from the house and −72 dBm from the garden
passes a fade test perfectly and looks exactly like something in a pocket. So
two more conditions, each answering a distinct way of being wrong:

| | catches |
|---|---|
| within 12 dB of its old strength | the house, audible but distant |
| heard at −70 dBm or better while away | things faint everywhere, which never fade |
| heard at least five times while away | a stranger passed once in a lane |

Together they say what the class claims: close by, the whole way. That is the distinction the whole scoring argument kept
circling: a follower *in a room* is the room, because the neighbour's phone
through a wall outlasts anything you can measure. A follower in two places,
with a walk in between, is not the room — it came too.

It scores six, so it alerts on its own, and it is the only class in the table
no signature can produce: the classifier cannot see it, and the tracker
promotes into it. Promotion is one-way, because something that has followed
you once has not stopped having done so by going briefly quiet.

A *journey* is sustained movement **that reached somewhere else**, and the
second half of that sentence was missing at first. Fifteen seconds of
carrying is the minimum, so a knocked desk does not count; but the
accelerometer can only say the board moved, never that it went anywhere, and
those are different questions. Carried around one house, it promoted every
follower in the building — ten of them in one evening — because nothing had
left: every device in range before was in range after.

So the **access points decide**. They are stationary by definition, there are
usually a dozen in earshot indoors, and the board already scans them every
fifteen seconds. The set in range is remembered when the board is picked up
and compared forty seconds after it settles, in two ways, because one of them
is not enough.

**Which ones are still there.** A third or fewer surviving means somewhere
else. In a town this is decisive: walk two streets and almost nothing
overlaps.

**How much fainter they have become.** Membership fails in the countryside,
where the only access points for half a mile are the ones in your own house
and they still reach the outbuildings. A barn at the end of a driveway kept
**80%** of them, so the journey did not count — correct by the rule and wrong
about the world. The same access points, all of them much fainter, is
distance: twelve decibels of median fade counts as having gone somewhere,
which is roughly four times as far away and far more than a person shifts by
turning round. A median rather than a mean, so one access point going quiet
behind a tractor cannot carry the answer.

The question is asked every twenty seconds while walking as well as on
settling, because standing still was never the point. Requiring the board to
be set down and left for the best part of a minute at the far end cost a
whole trip: a walk to a garden and back, with a pause too short to qualify,
was judged only on arriving home — where the access points were
twenty-five decibels *louder* than the mark taken in the garden, so nothing
counted at all. The board is somewhere else the moment the access points say
so.

Where there are no access points at all, nothing is counted and the log says
so — "you went nowhere" and "you arrived" would both be inventions, and the
quiet one is safer.

Nothing else uses the accelerometer: orientation tells you nothing about who
is nearby, and a screen that rotates is a screen that draws when nobody
asked.

The cost is stated plainly, because it will happen on the first walk: **your
own phone travels with you**, and a board that has not been told what is yours
will report it as tailing. Take a baseline before setting off. That is also
why the class is protected — a fingerprint rule may never silence it, and a
baseline quiets it one address at a time.

The eight boards with no accelerometer report zero journeys for ever, promote
nothing, and behave exactly as they did.

The screen lists the **heaviest finding first**, recency only breaking ties.
That was recency alone until a crowd of promotions filled all twelve rows of
a 466-pixel screen and a body camera would have gone off the bottom with
everything else.

- **0–2 clear** — nothing identified. LED winks once every 5 s (green)
- **3–5 caution** — equipment that could watch. LED pulses once a second (amber)
- **6+ alert** — equipment that is watching. LED flutters (red)

The rhythm is what the XIAO's single monochrome LED can say, and it is the same
on every board. Boards with an addressable WS2812, the C5 kits, add the
colour on top of it. They keep the rhythm rather than sitting lit, because a
device meant to sit unattended in a room should not also be a lit beacon
announcing itself; colour adds a second channel of information without making
the thing easier to spot. Brightness is deliberately low and is tunable under
`menuconfig`.

Adverts weaker than −90 dBm are discarded; they are far enough away to be
someone else's problem and they dominate the false-positive rate.

### Channels

The sniffer sweeps a list of channels, not a range, because 5 GHz channel
numbers are not contiguous.

**2.4 GHz — every chip.** Channels 1–13, swept in full every patrol cycle, with
the dwell split evenly across them. A beacon interval is typically ~102 ms, so
anything under about 120 ms per channel starts missing APs outright; 13
channels in a 5 s sweep leaves comfortable margin.

**5 GHz — ESP32-C5 only.** Twenty-five channels across UNII-1, UNII-2A, UNII-2C
and UNII-3. Sweeping all of them in one cycle would push the dwell under a
beacon interval, so 5 GHz is covered **a slice of five channels per cycle**,
advancing each sweep: 2.4 GHz stays fully covered every cycle and 5 GHz comes
round in five. The practical consequence is latency, not blindness. A 5 GHz
camera takes a few cycles longer to appear than a 2.4 GHz one.

DFS channels are included. The radar obligations that come with them apply to
transmitting, and this radio only ever listens. Channels the configured
regulatory domain refuses are skipped at runtime rather than being compiled
out, because the country setting is not known at build time.

### The radio is shared, and it shows

BLE and Wi-Fi share one radio. The coexistence arbiter divides it by the BLE
duty cycle, and the relationship is sharply non-linear. Measured on a XIAO
ESP32S3 over 40 s in a flat with 15 APs in range:

| BLE window/interval | duty | BLE sightings | Wi-Fi frames sniffed |
|---|---|---|---|
| 100/100 ms | 100% | 1490 | **2** |
| 60/160 ms | 37.5% | 925 | 199 |
| 45/160 ms | 28% | 747 | 223 |
| 30/160 ms | 18.75% | 608 | 278 |

A continuously-open BLE receiver does not slow the sniffer down, it starves it
outright. The default (60/160) gives up about a third of BLE throughput to get
a sniffer that works. Both values are tunable under `menuconfig` → **Observore**.

## Why there are modes

The ESP32-S3 has one radio on one channel. Channel-hopping to sniff and staying
associated to an access point are mutually exclusive, so Observore cannot both
watch the band and serve you a web page at the same time.

- **Patrol** — unassociated, scanning and sniffing. No network.
- **Uplink:** joined to your own network. The console is on your LAN and
  notifications can be sent. Wi-Fi sniffing is suspended.
- **Console** — SoftAP, for first-time setup or when away from your network.

BLE scanning continues in all three; it is unaffected by the Wi-Fi channel, and
it is where most detections come from.

| Gesture | Effect |
|---|---|
| hold 1.5 s | swap between patrol and uplink |
| hold 4 s | raise the console SoftAP |

On a board with a screen the short hold does something else: it **sets the
baseline**, and the screen says what it did — `baseline set: 14 now ignored`,
in amber, for a few seconds. That is the one action a person standing at the
device needs, and swapping modes by hand mattered only when the console was the
only way to see anything. The long hold still raises the console, which is
where the Wi-Fi gets configured.

With no network configured the short hold gives you the console instead, since
that is where a network gets configured. The firmware logs how long it saw the
button held, so a press a shade too short is distinguishable from a button that
is not responding.

### Patrol and uplink alternate

**All Wi-Fi detection — the access-point scan and the promiscuous sniff both —
runs only while patrolling.** A device parked permanently on the uplink is a
BLE-only detector: no Remote ID drones, no hidden access points. So Observore
alternates.

| Setting | Default | Meaning |
|---|---|---|
| `OBSERVORE_PATROL_WINDOW_S` | 120 s | patrolling, full sensor |
| `OBSERVORE_UPLINK_WINDOW_S` | 30 s | on your network, pushing and serving |
| `OBSERVORE_UPLINK_MAX_S` | 180 s | hard ceiling on one uplink visit |
| `OBSERVORE_CONSOLE_IDLE_S` | 45 s | console silence before the window may close |
| `OBSERVORE_UPLINK_GRACE_S` | 45 s | tolerate a dropped uplink before patrolling |
| `OBSERVORE_UPLINK_RETRY_S` | 300 s | backoff after a *failed* join |

The uplink window is held open while somebody is reading the console, so you
are never cut off mid-page, but only up to `OBSERVORE_UPLINK_MAX_S`. The
console page polls every two seconds, so a tab left open would otherwise keep
the device on the uplink indefinitely. **A user interface must not be able to
blind the detector**, so the hold has a hard ceiling.

The console is therefore reachable within about two minutes rather than
continuously. If it does not answer, it is patrolling: wait for the next uplink
window, or hold the button for 1.5 s to go there now.

The button means "switch now" and alternation continues from there, so a single
press can never strand the device in a mode it will not leave.

## Joining your network

Uplink mode puts the console on your LAN, so you can read the log without the
SoftAP dance. **It costs detection**: while associated, the radio is pinned to
your AP's channel and the Wi-Fi sniffer is suspended. BLE scanning and AP scans
continue. If the network cannot be joined, Observore falls back to patrol rather
than sitting associated to nothing.

Credentials never belong in a tracked file. There are three ways to set them,
and the first is the one to prefer:

**1. At runtime, from the console.** Hold the button for 4 s to raise the
SoftAP (1.5 s is enough before any network is configured), join it, and fill in
the **Network** panel. Stored in NVS. Nothing touches this repo at all, and
nothing needs rebuilding to re-point a device at a different network.

**2. `idf.py menuconfig`** → **Observore** → Wi-Fi uplink. This writes `sdkconfig`,
which is gitignored.

**3. A gitignored `credentials.conf`**, for reproducible or CI builds:

```bash
cp credentials.conf.example credentials.conf   # gitignored
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;credentials.conf" build
```

A credential in NVS wins over one compiled in, so setting it at runtime is not
silently reverted by reflashing the same firmware.

**Leaving the password field blank keeps the stored password.** The console
clears the field after every save and never receives the password back, so
always sending it would let a second Save replace a good password with an empty
one, which asks for an *open* network, and a WPA2 access point refuses that
with `reason 210, no AP found with compatible security`. That reads as though
the network were at fault. To genuinely configure an open network, tick **open
network**.

The panel shows whether a password is stored (`password set` / `NO PASSWORD`)
without ever revealing it.

### When the uplink will not join

The console's Network panel and the serial log both give the reason in words,
and every attempt is logged rather than only the last — logging only the last
reported `reason 36`, which is Observore's own disconnect in the timeout path and
says nothing about the real cause.

| Reason | Meaning |
|---|---|
| 210 | security mismatch — most often **no password stored** |
| 201 | network not found — check the SSID, and that the band is one your chip has |
| 202, 15, 204 | authentication or handshake failed — wrong password |
| 203 | association refused — MAC filtering? |

The ESP32-S3 and C6 have no 5 GHz radio, so a 5 GHz-only SSID can never be
joined on those chips. A C5 can join either band.

### Provisioning from the browser (Improv)

The quickest way onto a network is to let the browser that just flashed the
device also configure it, over the same USB cable, using
[Improv Serial](https://www.improv-wifi.com/serial/). The
[flasher page](https://observore.forgedinfeatherstechnology.com/) offers it
straight after installing.

That matters more than convenience. The console password is printed on the
serial log and nowhere else, so **a device you flash for somebody else is a
device they cannot configure** — they have no way to read the password, so
they cannot join the setup SoftAP, so they cannot enter their Wi-Fi. Improv
removes the password from the setup path entirely: it is still needed to open
the console later, but no longer to get the device onto a network.

From a terminal, without a browser:

```bash
tools/improv_client.py --port /dev/ttyACM0 info
tools/improv_client.py --port /dev/ttyACM0 provision --ssid MyAP --password secret
```

Three things worth knowing:

- **Serial only, deliberately:** Improv also defines a BLE transport, and it is
  the wrong choice here: it would make a counter-surveillance detector
  advertise. This one only answers on a cable somebody has physically plugged
  in.
- **Both USB sockets work on a C5:** the console can only *read* from one of
  them — ESP-IDF's secondary console is output-only, so the firmware talks to
  each peripheral directly rather than through stdio. Whichever socket you used
  to flash is the one that provisions. Both are verified on hardware.
- **Provisioning drops the existing link first:** asking to join while already
  joined is a no-op that reports success, which would accept a wrong password
  without ever trying it.
- **A failed join costs nothing:** credentials must be stored before the
  station can try them, so a mistyped password would otherwise replace a
  working network with a broken one. The previous network is put back if the
  join fails.

The offered network list comes from the patrol sweep that is already running,
rather than from a scan started on demand — this chip has one radio, and a scan
requested here would fight the sweep for it. An unconfigured device patrols, so
the list fills within a cycle of boot; before that first sweep completes the
list is empty and the SSID can be typed instead.

### Finding the device on your network

Observore offers the name **`observore`** (configurable as
`OBSERVORE_HOSTNAME`) two ways: over mDNS as `observore.local`, and as the
hostname in its DHCP request, which is what a router registers in its own DNS
and shows in its client list.

The DHCP one is the more useful across subnets, because ordinary DNS routes and
multicast does not.

Two caveats, both real:

- **mDNS is link-local multicast and does not route between subnets:** if
  Observore sits on an isolated VLAN and you browse from the main LAN, the
  `.local` name will not resolve unless your router reflects mDNS across both
  networks — on UniFi that is the *Multicast DNS* setting, and it must be
  enabled on each network, not just one.
- **A DNS domain ending in `.local` collides with mDNS:** RFC 6762 reserves
  `.local` for multicast, so most resolvers send `*.local` to mDNS and never
  ask your DNS server. If your LAN domain is something like `house.local`,
  names under it are ambiguous for every client, not just this one. A DHCP
  reservation plus a static record under a non-`.local` domain sidesteps the
  whole problem.
- **Some clients cannot resolve `.local` at all:** a Linux box with no Avahi
  and `systemd-resolved` showing `-mDNS` has no multicast resolver, so the name
  will fail there however the network is configured.
- **It is only on the network during its uplink window:** while patrolling it
  has no address at all, so neither the name nor the IP will answer. Wait for
  the next window, or hold the button for 1.5 s.

The address is also reported on the serial log as `uplink up at <ip>`, in the
console's Network panel, and in your router's DHCP lease table. The Wi-Fi
station MAC is printed at boot.

Set `OBSERVORE_WIFI_AUTOJOIN=n` to keep full patrol coverage and reach the uplink
only on demand via the button.

### Keeping credentials out of the repo

`CONFIG_OBSERVORE_WIFI_SSID` and `CONFIG_OBSERVORE_WIFI_PASSWORD` are empty in the
tracked `sdkconfig.defaults` and must stay that way. The easy mistake is to set
one with menuconfig, then paste it into `sdkconfig.defaults` to make it stick —
which is how a home network password ends up in a public repository.

```bash
tools/check_no_secrets.sh            # scan tracked files
tools/check_no_secrets.sh --staged   # scan what is about to be committed
```

Wire it up as a pre-commit hook if you intend to push this anywhere:

```bash
printf '#!/bin/sh\nexec tools/check_no_secrets.sh --staged\n' \
  > .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
```

The password is write-only from outside the device: no API returns it, and the
console never receives it, so the field stays blank even when a network is
configured.

### Nothing left behind

The other local check guards a subtler thing than a leaked password: a commit
that left part of itself behind.

```bash
tools/check_nothing_left_behind.sh            # list what is uncommitted
tools/check_nothing_left_behind.sh --strict   # and fail
```

```bash
printf '#!/bin/sh\nexec tools/check_nothing_left_behind.sh --strict\n' \
  > .git/hooks/pre-push && chmod +x .git/hooks/pre-push
```

It exists because of one commit here. A feature was staged as `git add main
boards README.md`, its test file was not on that list, and the feature reached
`main` with none of its coverage. CI passed: it ran the old tests, they were
green, and the checks that would have proved the new code works were never
there to run. **A green tick reported the absence of a test as the success of
one**, which is the most expensive kind of wrong a build can be.

No amount of care fixes that, because the habit causing it — naming paths to
`git add` — is fast precisely by being unexamined. So the check is mechanical:
after a commit, is anything still uncommitted that looks like part of it? It
deliberately ignores build output and scratch files, and deliberately notices
an untracked file under `main/`, `test/`, `tools/` or `boards/` — a new module
written, committed nowhere, and building fine locally because the file is on
disk.

It cannot run in CI, which is the point: CI only ever sees what was pushed,
and this is a check about what was not.

## Building

For a first run, follow [Getting started](#getting-started). This section is
the reference for everything after that.

### Flashing a release without a toolchain

Every tagged release carries a bootloader, partition table and application
**per supported chip**, named with the target, plus a `SHA256SUMS`, an [ESP Web
Tools](https://esphome.github.io/esp-web-tools/) `manifest.json` and the
per-target `builds-<target>.json` the manifest is assembled from.

The browser installer decides whether it is looking at an upgrade or a fresh
install by asking the board what it is running. If it finds Observore, it
offers an *Update*, which writes the firmware and touches nothing else. If it
finds something else, or nothing that answers, it asks whether to erase first,
and the box starts unticked.

**Releases before v0.7.0 got this wrong**, and if you ever set a device up
twice this is why. The installer matches on an exact name, the manifests
carried the board label in theirs, and the firmware did not, so every install
looked like a fresh one. With the erase prompt turned off, on the belief that
off meant safe, the installer erased without asking. Both settings were read
from the installer's source before being changed, and the page says what it
does now.

With `esptool` alone — note that **the bootloader offset is not the same on
every chip**, so these commands are not interchangeable:

```bash
# Seeed XIAO ESP32S3
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
    0x0     bootloader-xiao-esp32s3.bin \
    0x8000  partition-table-xiao-esp32s3.bin \
    0x10000 ota_data_initial-xiao-esp32s3.bin \
    0x20000 observore-xiao-esp32s3.bin

# ESP32-C5-DevKitC-1 / Waveshare
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash \
    0x2000  bootloader-devkit-esp32c5.bin \
    0x8000  partition-table-devkit-esp32c5.bin \
    0x10000 ota_data_initial-devkit-esp32c5.bin \
    0x20000 observore-devkit-esp32c5.bin

# Seeed XIAO ESP32-C5
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash \
    0x2000  bootloader-xiao-esp32c5.bin \
    0x8000  partition-table-xiao-esp32c5.bin \
    0x10000 ota_data_initial-xiao-esp32c5.bin \
    0x20000 observore-xiao-esp32c5.bin

# Seeed XIAO ESP32C6
esptool.py --chip esp32c6 -p /dev/ttyACM0 write_flash \
    0x0     bootloader-xiao-esp32c6.bin \
    0x8000  partition-table-xiao-esp32c6.bin \
    0x10000 ota_data_initial-xiao-esp32c6.bin \
    0x20000 observore-xiao-esp32c6.bin

# ESP32-2432S028R (2.8" CYD, ST7789 revision) -- a CH340 bridge, so ttyUSB
esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash \
    0x1000  bootloader-cyd-2432s028r-st7789.bin \
    0x8000  partition-table-cyd-2432s028r-st7789.bin \
    0x10000 ota_data_initial-cyd-2432s028r-st7789.bin \
    0x20000 observore-cyd-2432s028r-st7789.bin

# ESP32-2432S028R (2.8" CYD, ILI9341 revision) -- same board, other panel
esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash \
    0x1000  bootloader-cyd-2432s028r-ili9341.bin \
    0x8000  partition-table-cyd-2432s028r-ili9341.bin \
    0x10000 ota_data_initial-cyd-2432s028r-ili9341.bin \
    0x20000 observore-cyd-2432s028r-ili9341.bin

# ESP32-3248S035R (3.5" CYD, resistive touch)
esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash \
    0x1000  bootloader-cyd-3248s035r-st7796.bin \
    0x8000  partition-table-cyd-3248s035r-st7796.bin \
    0x10000 ota_data_initial-cyd-3248s035r-st7796.bin \
    0x20000 observore-cyd-3248s035r-st7796.bin

# RockBase NM-CYD-C5 (2.8" CYD on a C5) -- native USB, so ttyACM
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash \
    0x2000  bootloader-nm-cyd-c5.bin \
    0x8000  partition-table-nm-cyd-c5.bin \
    0x10000 ota_data_initial-nm-cyd-c5.bin \
    0x20000 observore-nm-cyd-c5.bin

# Waveshare ESP32-S3-Touch-AMOLED-1.43 (round, capacitive)
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
    0x0     bootloader-waveshare-s3-amoled-143.bin \
    0x8000  partition-table-waveshare-s3-amoled-143.bin \
    0x10000 ota_data_initial-waveshare-s3-amoled-143.bin \
    0x20000 observore-waveshare-s3-amoled-143.bin
```

`manifest.json` in the release is the authoritative copy of those offsets: it
is generated from each build rather than written by hand, so if these ever
disagree, believe the manifest.

**These are three separate files on purpose.** A single merged image would
span `0x0` upward with the gaps padded, and the NVS partition sits at `0x9000`
— inside that span. Flashing one would silently erase every mute rule, the
Wi-Fi credentials, the notifier token and the device's generated console
password. Writing the parts at their own offsets leaves NVS alone, so an
upgrade keeps everything it has learned and wiping is an explicit choice
(`esptool.py erase_flash`).

```bash
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py set-target esp32s3`, or `esp32c5`, or `esp32c6` — is needed once in
a fresh checkout, and again whenever you change target.

### Other targets

The reference board is the XIAO ESP32S3 and every measurement here was taken on
it. The firmware also builds for `esp32c5` and `esp32c6`, and CI builds all
three so portability breaks surface immediately rather than months later.

The **ESP32-C5 is the interesting one**, because it is dual-band: it is the only
supported chip that can see 5 GHz at all. On a C5 the sniffer sweeps both bands
(see [Channels](#channels)); on every other chip 5 GHz is simply invisible.

### Boards, which are not the same as chips

`sdkconfig.defaults.<target>` describes a chip. A **board** is a separate thing
and cannot be inferred from it — the ESP32-C5 appears three times here: as a
Waveshare kit with a WS2812 and a CH343 UART bridge, as a XIAO with a plain
LED and only native USB, and as a Cheap Yellow Display with a screen on it. Board files live in [`boards/`](boards/):

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/xiao-esp32c5.defaults" \
       --preview set-target esp32c5 build
```

| board | LED | button | notes |
|---|---|---|---|
| XIAO ESP32S3 *(default)* | GPIO21, plain | GPIO0 | the reference board |
| ESP32-C5 DevKitC / Waveshare *(default for C5)* | GPIO27, WS2812 | GPIO28 | two USB sockets, either works |
| `boards/xiao-esp32c5` | GPIO27, plain | GPIO28 | 8 MB PSRAM, native USB only |
| `boards/xiao-esp32c6` | GPIO15, plain | GPIO9 | **no PSRAM**, so TLS is tight |
| `boards/cyd-2432s028r-st7789` | GPIO16, plain (green of the RGB) | GPIO0 | original ESP32, **no PSRAM, no notifier**; the later 2.8" CYD, on-screen display |
| `boards/cyd-2432s028r-ili9341` | GPIO16, plain (green of the RGB) | GPIO0 | as above for the original 2.8" CYD and its 2.4"/3.2" siblings; **not yet confirmed on hardware** |
| `boards/cyd-3248s035r-st7796` | GPIO16, plain (green of the RGB) | GPIO0 | the 3.5" CYD, 480x320 with touch; resistive **R** model only |
| `boards/nm-cyd-c5` | GPIO27, WS2812 | GPIO28 | the 2.8" CYD on a **C5**: 8 MB PSRAM, dual-band, with touch. The only display board that sees 5 GHz |
| `boards/waveshare-s3-amoled-143` | none — the panel is the indicator | GPIO0 | round 466x466 AMOLED, capacitive touch, clock and **accelerometer** |

The XIAO C5 is the awkward one: its LED is on **the same pin** as the DevKitC's
addressable pixel but is an ordinary LED, so getting the board wrong leaves it
dark rather than obviously broken.

The XIAO profiles are **compile-tested only**. Neither board has been run.
[docs/xiao-verification.md](docs/xiao-verification.md) lists what is unverified
and how to check it, ordered by how likely each is to be wrong.

The [browser flasher](https://observore.forgedinfeatherstechnology.com/) ships
a build per board and asks which one you have, because it cannot tell. ESP Web
Tools matches on chip family, which separates an S3 from a C5 and cannot
separate two C5 boards, so the picker handles boards that share a chip, and
the chip check remains underneath as a backstop: choosing a profile for the
wrong *chip* is refused rather than flashed.

Building without a board file gives the reference board for that chip.

Per-chip settings live in `sdkconfig.defaults.<target>`, which ESP-IDF loads on
top of the shared `sdkconfig.defaults`. What differs in practice:

| | XIAO ESP32S3 | ESP32-C5 | ESP32-C6 |
|---|---|---|---|
| Bands | 2.4 GHz | 2.4 + 5 GHz | 2.4 GHz |
| LED | one GPIO, active low | WS2812 pixel | board-dependent |
| LED / button GPIO | 21 / 0 | 27 / 28 | set them yourself |
| PSRAM | 8 MB | on `R` modules only | none |

A RISC-V build is about a fifth larger than Xtensa, which is why the
application partition is sized the way it is.

The C5 image is built with `SPIRAM_IGNORE_NOTFOUND`, so one binary boots on
boards with PSRAM and without: the console sizes its scratch from what it can
actually allocate. That matters because the browser flasher cannot tell an
`N16R8` from a module with no PSRAM at all.

**On hardware verification:** the S3 and the C5 are both verified on real
hardware. C6 is compile-tested only.

The C5 was verified on a Waveshare ESP32-C5-WIFI6-KIT-N16R8, flashed with the
released artifacts rather than a local build, so the release pipeline is
covered too. Confirmed working: 8 MB PSRAM detected and tested, dual-band scan,
BLE passive scan, Wi-Fi uplink, mDNS, the web console including a 6 KB
`/api/nearby` response, and Set baseline.

The WS2812 and the button were checked directly rather than assumed. The pixel
was driven through a known red-green-blue sequence and observed in that order,
which rules out the failure actually worth worrying about: blue is the third
byte in both RGB and GRB ordering, so a wrong colour format looks *correct* on
blue while silently swapping red and green. An alert would show green. It does
not. The BOOT button on GPIO28 reads high at rest and low when pressed.

**The dual-band result, measured in an ordinary flat:**

| | 2.4 GHz APs | 5 GHz APs | total |
|---|---|---|---|
| XIAO ESP32S3 | 15 | — | 15 |
| ESP32-C5 | 12–15 | 15–17 | 27–30 |

Slightly more than double, and the 2.4 GHz counts agree between the two chips,
which is what makes the extra APs credible rather than an artefact. Everything
in the 5 GHz column was previously invisible to this project.

The BLE/Wi-Fi coexistence table below is still an S3 measurement. At the
default 60/160 duty the C5 sniffed a comparable number of management frames per
sweep, so the shape carries over, but the table has not been re-measured
point-by-point on that chip.

The XIAO uses the S3's native USB-Serial/JTAG, so it enumerates as
`/dev/ttyACM0`, not `/dev/ttyUSB0`.

Everything tunable lives under `idf.py menuconfig` → **Observore**: the LED and
button pins, the hostname, the patrol and uplink windows, the BLE duty cycle,
and the credential overrides. Settings written there land in `sdkconfig`, which
is gitignored.

### The console password

It is used twice: as the SoftAP's WPA2 key, and to unlock the console when the
device is on your own network.

Each device generates its own on first boot, stores it in NVS and prints it on
the serial log:

```
console SoftAP: "console-XXXXXX"  password: xxxxxxxxxxxx
```

It is not derived from anything observable and it is not shipped in this
repository, so no two devices share one. Deriving it from the MAC would be
pointless. The SoftAP's BSSID is in every Wi-Fi scan and the derivation would
be right here in the source. A build-time constant would be worse still:
everyone who flashed the firmware would share it.

It is deliberately **not** exposed over the network. Serving the console's own
password from inside the console would turn "someone was on the LAN once" into
"someone can join the SoftAP in range, indefinitely". Read it from serial.

#### Rotating it

The password lives in the `ap_pass` NVS key and is regenerated whenever that
key is absent, so erasing NVS is what rotates it:

```bash
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash monitor
```

That erases everything else in NVS too — the uplink credentials, the notifier
settings and your mute rules. The baseline is the cheapest of those to lose: it
is regenerated by pressing **Set baseline** once, which is what it did the first
time. Manually added mute rules are the ones worth writing down first.

Rotate if the password has been seen by anyone who should not have it — a
screenshot, a pasted log, or a serial capture shared for debugging. Treat it
like a router's Wi-Fi key, because that is exactly what it is.

`OBSERVORE_AP_PASSWORD` overrides it, and every device built from that firmware
then shares the password you chose. `tools/check_no_secrets.sh` fails if such a
value reaches a tracked file.

#### Unlocking the console

On your own network the console asks for that password once and exchanges it
for a session cookie, so the password is not repeated on every request, which
matters, because it is also the WPA2 key to the device's own access point. The
session lasts two hours of use and **Lock** ends it.

The SoftAP is deliberately not challenged. WPA2 already authenticates that link
with the same password, and first-time setup is the one moment the console is
the only way in.

Everything except the page itself and the login endpoints requires a session:
the detection log, the mute rules, and the network and notifier settings. The
requirement lives in the route table and a single dispatcher enforces it, so a
route added later cannot quietly forget to check.

Repeated wrong guesses are slowed by a delay that grows with consecutive
failures. Set `OBSERVORE_CONSOLE_AUTH=n` to turn the whole thing off, which is
reasonable only on a network you would be content to leave the detection log
readable on.

**This is authentication, not encryption.** Over plain HTTP the session cookie
can be captured by anything sniffing the LAN.

### Reading the log

Once a network is configured the console lives on your LAN, and the SoftAP is
only needed when you are away from it — hold the button for **4 s** to raise it,
join `console-XXXXXX`, and open <http://192.168.4.1/>. The page lists every classified device with its class,
MAC, signal, evidence and how long ago it was last heard.

### Tests

The classification, parsing and scoring logic builds and runs on the host with
no hardware and no ESP-IDF:

```bash
make -C test test
```

CI runs these on every push, alongside the credential scan, an ESP-IDF build
for every supported target, a check that this README and the flasher page still
describe the release the build actually produces, and a validation of the
generated OUI table — that both tables
are sorted for binary search, carry no duplicates, stay disjoint from each
other, and index only names that exist.

That check deliberately does not re-download the IEEE registry to diff against.
The registry is unreachable from GitHub's runners, and it publishes new
assignments constantly, so such a job would fail for reasons unrelated to
whether this repository is correct. Refreshing the table stays a deliberate
manual step.

### Regenerating the OUI table

`main/observore_oui_table.h` is generated, not hand-maintained. Vendors get new
prefixes; refresh it with:

```bash
python3 tools/gen_oui_table.py
```

Both tables live in that one generated header. The threat categories are in
`CATEGORIES` and the benign vendor list is in `VENDORS` — add vendors there,
not to the generated header. A prefix claimed as a threat is never also listed
as benign.

Each benign prefix costs 4 bytes of flash (the name is an index into a shared
table, not a pointer per row). The 43 vendors shipped cost about 40 KB of
flash. Adding more is cheap; trimming `VENDORS` is the way to claw it back.
`idf.py build` reports how much of the app partition remains.

## Ignoring what you already know about

The console's **Nearby, unidentified** panel lists everything that matched no
threat signature, busiest first, with a vendor name where one is known and an
**ignore** button on each row. That is the intended workflow: name your own
gear once, ignore it, and let what remains stand out.

How well vendor naming works depends entirely on the radio:

| Source | Vendor resolution | Why |
|---|---|---|
| Wi-Fi AP BSSIDs | good | access points use their real assigned MAC |
| Wi-Fi virtual BSSIDs | none | guest/IoT SSIDs use locally-administered addresses |
| Wi-Fi clients | usually none | modern devices randomise when probing |
| BLE | mostly none | phones and trackers rotate their address |

A blank vendor is reported honestly as either **random MAC** (the device is
deliberately anonymous, and ignoring it only holds until it rotates) or
**unknown** (a genuine gap in the table). Conflating those two would make the
UI lie about what it knows.


A detector that cries wolf at your own doorbell every day is one you stop
reading. Anything already judged harmless can be muted, on either radio, at
four levels of breadth:

| Rule | Ignores | Survives MAC rotation | Use it for |
|---|---|---|---|
| `name` | a name or SSID substring | **yes** | anything that broadcasts a name |
| `fingerprint` | the stable shape of a BLE advert | **yes** | nameless BLE devices |
| `mac` | one exact address | no | a device with a fixed address |
| `oui` | a whole vendor prefix | n/a | a neighbour's camera brand |
| `class` | an entire class | n/a | every `camera` on a busy street |

**Why rotation matters.** Most BLE devices change their address every few
minutes to an hour, so a `mac` rule silences a device only until it rotates.
Measured here, all fourteen nearby BLE devices used rotating addresses. A
MAC-based baseline would have been worthless within the hour.

### Things bolted to the building

A solar gateway that broadcasts `Envoy / 1219…` from a fixed address, eighty-
eight decibels down, was being reported as a **follower** — a class defined as
"unidentified but persistently nearby", when it is neither unidentified nor
going anywhere. Enphase gear is now named as a **`fixture`**, worth no points,
and that is better than muting it: an ignore rule hides a thing, a class
explains it, and the explanation survives **Clear ignores**.

The general half matters more than the vendor. Anything publishing both a fixed
public address and a stable broadcast name has opted out of being hard to
identify, so persistence alone says much less about it — that is what a printer
looks like, not what something trying not to be noticed looks like.

Such a device is held to **an hour** before persistence promotes it, rather
than the five minutes a nameless or rotating one gets. Not an exemption, and
the distinction is the point: a cheap tracker with a fixed name from a public
address would still be following you, and an hour beside you is worth a word
whatever the thing calls itself. Five minutes catches the fixture in the next
room; an hour does not.

### Equipment that transmits at other radios

Everything else here is equipment that watches. This is the other kind, and it
is worth separating.

**A deauthentication flood** is the most actionable thing on Wi-Fi this device
can see. One deauth is ordinary — access points dismiss clients all day — but a
burst of them is how a handshake is forced into the air to be captured, and how
a camera is taken offline shortly before something happens in front of it. No
vendor prefix can hide it, because the tell is behaviour rather than hardware:
eight or more such frames naming one address inside ten seconds, reported once
a minute at most.

It names a victim, not a culprit. An attacker spoofs the access point's
address, so what is reported is the address the flood was sent as. Claiming
otherwise would be inventing an attribution the air does not carry. And the
sniffer sits on one channel for a few seconds at a time, so a flood elsewhere
in the band is simply missed — this finds what passes under the aerial and
says nothing about what does not.

**Hunter gear** — a Flipper Zero, a pwnagotchi, a WiFi Pineapple — scores
three rather than six: the presence of a tool is capability, where a flood is
an act. So hunter gear is caution on its own and alert beside something else.

Two of those signatures are worth their footnotes. Flipper's Bluetooth company
ID is `0x0E29`; the widely copied `0x0FBA` belongs to Cosonic, who make
headsets, so every project carrying that constant reports their customers as
hacking tools. And a pwnagotchi volunteers everything — it puts a plain-ASCII
JSON blob in a vendor element of its own beacons so other pwnagotchis can find
it — so the signature is one byte scan for `pwnd_tot`, with no JSON parser
anywhere near the promiscuous callback.

### Earbuds are not trackers

Anything advertising Google's Fast Pair service used to arrive as a `tracker`
worth three points — which is every pair of budget earbuds in pairing mode, and
how a crowded cafe reads as four trackers. A person who sees that a few times
stops believing the score.

A Fast Pair advert carrying exactly three bytes of service data is the
discoverable frame: a 24-bit model ID, which is what a device announcing itself
to pair sends. That is now an **`accessory`** at one point. Everything else
under the same UUID keeps full tracker weight.

The asymmetry is deliberate, because no byte in this advert reliably tells a
tag from a headphone. Mistaking a tracker for an accessory costs two points on
a device that is **still reported and still listed**; mistaking it for nothing
would cost the detection. Nothing is dropped here — only weighted — and a
tracker in pairing mode is one its owner is setting up, not one following
somebody.

### Other detectors

[SquachWatch](https://squachwatch.com) is another open-source detector on the
same board family, and in its mesh mode it announces itself so that two of them
can recognise each other. Observore reports one as **`peer-detector`**, named
`SquachWatch`, with whatever name its owner typed.

It is worth **zero points** on purpose. Another detector in the room is a fact
about the room rather than a threat in it, and a class that moved the score
would make a meetup read as an incident. It is still announced, because
somebody else watching the same street is exactly the sort of thing a person
wants to know.

The signature was read out of their source rather than inferred from the air,
and the distinction matters. The payload rides in manufacturer data under
company ID `0xFFFF` — the Bluetooth SIG's reserved non-production ID, which is
shared with every other hobby project that declined to squat a registered one,
so the company alone means nothing. The four magic bytes `SQM1` are what make
it specific, and their own header says the magic is not optional for that
reason. A typed name is twelve bytes at a fixed offset, and it is repeated only
while every byte of it is printable: half of somebody's chosen name is not
their name, and an escape sequence in a field this device draws would be a
stranger deciding what our screen does.

A **fingerprint** hashes only the parts of an advert that survive rotation:
which AD fields are present and how long they are, the manufacturer's company
ID, the service UUIDs, and the local name. The variable payload is excluded, so
a Find My advert fingerprints identically before and after it rotates its key.
It is not perfectly stable — a device that varies its advert *structure* gets a
new fingerprint, but it holds for the large majority.

**Ignoring something removes it now.** Muting suppresses what arrives next and
says nothing about what is already in the device table, which used to sit there
until it aged out half an hour later — on a screen that reads as the ignore
having failed, because the thing you just dismissed is still in front of you.
Adding a rule now sweeps the table so the two agree at the moment of the
decision. The sweep asks the matcher a non-counting question, because charging
those rows to whichever rule covered them would inflate what each rule appears
to have suppressed, and could retire a good rule for a population it never
actually silenced.

**A fingerprint identifies a kind of device, not an individual one.** Two
identical trackers fingerprint the same. So a fingerprint rule is never allowed
to silence a `tracker`, `bodycam`, `alpr`, `drone`, `smart-glasses` or
`follower` — muting your own AirTag that way would silence a stranger's too,
which is the exact thing this device exists to notice. The rule is enforced
inside `observore_mute_matches()` rather than left to callers, and there is a test
pinning it.

Muted sightings are suppressed before they reach the device table: they are not
logged, not scored, and cannot be promoted by the follower heuristic. A `class`
rule deliberately never matches unclassified traffic, so muting `camera` does
not quietly switch off follower detection.

Rules are stored in NVS and survive a reboot. Manage them from the **Ignored**
panel in the console, or over the API:

```
GET  /api/mutes
POST /api/mute?mac=AA:BB:CC:DD:EE:FF
POST /api/mute?oui=AA:BB:CC
POST /api/mute?class=camera
POST /api/mute?ssid=lobby
POST /api/unmute?index=0
POST /api/unmute?all=1
```

Each row in the device table also has an **ignore** button, which is the usual
way to add one. The row disappears on click rather than on the next poll, and
comes back with an error if the device rejects it.

### Set baseline

**Set baseline** marks everything currently in range as known and resets the
score, so the device starts watching for what changes from *here* rather than
reporting the whole neighbourhood. Run it where it will live, and what it flags afterwards is genuinely new.
"In range" means everything in the device table, which holds a device for
thirty minutes after it was last heard — so a baseline taken within half an
hour of an alert includes the device that raised it, whether or not it is still
advertising at that moment.

It mutes detected threats too, which is the point: your own doorbell camera is
exactly the thing you want silenced. It asks for confirmation once, and
**Clear ignores** undoes all of it.

Baseline picks the most durable rule each device supports: its name if it
broadcasts one, else its advert fingerprint, else — only as a last resort — its
MAC. It reports the breakdown, and counts how many rules are merely temporary
because they had to fall back to a rotating address.

**A baseline must never make the device deaf**, and until v0.9.1 it could.
Pressed in a house, it wrote fingerprint rules for shapes that half the
Bluetooth devices in the building broadcast, and the detector went from three
thousand sightings an hour to twenty-five while still reporting "clear" —
which is indistinguishable from a quiet room. It took two boards side by side
to notice: one admitted eight thousand sightings, the other, two metres away,
admitted six.

Four things now stand between a baseline and that outcome.

A fingerprint rule is written **only for a shape carried by exactly one device
in range at that moment**. Two devices sharing a shape means the shape is a
model, not a device, and both are muted by address instead. The baseline makes
two passes over the table for this, because a single pass cannot know whether
what it is looking at is shared.

A name rule needs a name of at least four characters. Name rules match as
substrings, which is right when a person types one and wrong when a baseline
takes whatever it hears: the SSID `42` silenced a hundred different addresses
here, each one something whose name merely contained those two characters.

**A protected class is silenced by address and nothing else**, and this one
was learned late. Protection used to be checked only on the fingerprint
branch, and the name branch ran first — so anything broadcasting a name of
four characters or more walked straight past it, and the comment next to the
code promised the opposite. A Flipper Zero went unreported on two boards for a
week because a baseline had turned `Flipper Arala75h` into a name rule, and it
took a third board with an empty rule table, hearing the same Flipper from the
same desk, to see it. The cost of that bug scales badly: a body camera
broadcasting `AXON BODY 3` would have left a substring rule quieting every
Axon Body 3 the owner ever walked past. Trackers, body cameras, plate readers,
drones, smart glasses, hunter gear and deauthentication floods now get a rule
naming one address, which silences the device in front of you and nothing
else. **And the baseline says when it has done it** — `baseline: 14 ignored,
incl 1 hunter` on the glass, a `protected` count on `/api/baseline`, and a
sentence in the console. Silencing the thing the device exists to find should
never be something you discover a week later.

And **every rule reports what it has actually done** — how many sightings it
has discarded and how many distinct addresses it has covered, on `/api/mutes`
and in the console beside the rule. A fingerprint rule that turns out to cover
more than eight addresses is describing a population rather than a device, so
the device stops honouring it and says so. That last one is the safety net for
the case the first two cannot see: a shape shared with devices that were not
in the room when the baseline was taken.

One exception to both rules above is made here, and only here. A `follower`
may be silenced by name, and one on a rotating address gets a fingerprint rule
from a baseline where a console mute would refuse one. "Follower" is a verdict about how long
something has been near you, not about what it is; in your own home the device
that has been near you for five minutes is your phone, and a MAC rule for it
dies at its next rotation — which is how the same handset ends up announced
every day. A baseline is you standing at the device saying that what is here
now is yours, and your phones are the whole point of that. The cost is stated
plainly: a stranger carrying the same model, advertising the same way, is quiet
too. Trackers, drones, body-worn cameras and the rest keep their protection,
because they are classified by what they are, and a baseline does not change
what they are. A `follower` that keeps a fixed address gets the MAC rule, which
is both more specific and just as durable.

Dismissing a finding by tapping it on the glass follows the same rule, for the
same reason: the tap silences the body camera in front of you, not every one
of that model you will ever walk past.

Measured on real air: fourteen devices in range, all of them rotating their
addresses, produced three name rules and ten fingerprint rules and **zero**
MAC rules. A minute later, after rotation, the score was still zero.

Up to 128 rules are stored, in NVS, surviving reboots.

### What a drone broadcasts about itself

A drone complying with ASTM F3411 / Remote ID transmits, in clear and
unauthenticated, its own position and altitude, its speed and heading, its
serial number, and — the part that surprises people — **the location of its
operator**. Regulation requires it, which is why it is there to be read.

Observore recognised these broadcasts and labelled them `drone`, then threw the
payload away. It now decodes it, and the finding carries where the pilot is
standing:

```
drone   Remote ID drone   pilot 51.5080,-0.1290   -55 dBm
```

The operator's position leads where both are known, because it is the one thing
you could not have worked out by looking up.

**Silence is not evidence of no drone.** Consumer aircraft broadcast Remote ID
because they are obliged to; anything that does not want to be found does not
transmit it, and these radios see only 2.4 GHz Wi-Fi and BLE in any case. A
quiet screen means *nothing announced itself*, never *the sky is clear*. If this
is ever relied on for physical safety, that distinction is the whole thing.

Three details of the decoding are worth recording, because each is a way it
could have been quietly wrong:

- **The offsets came from the reference header, not from memory.** A first draft
  placed latitude at bytes 4–7. It is at 5–8, because `SpeedVertical` takes
  byte 4. That one-byte error yields coordinates that look entirely plausible
  and are wrong by continents, and the host tests now fail on it.
- **Zero latitude and zero longitude is refused.** It is what a drone transmits
  before it has a fix, and it is also a real place in the Gulf of Guinea.
  Reporting it would be a confident lie about where something is, which is
  worse than saying nothing.
- **A pack that lies about its own geometry is refused rather than walked.** A
  message pack declares its stride and how many messages follow; a broadcast is
  not a trustworthy narrator, so both are checked against the format and
  against how many bytes actually arrived.

Positions are kept as the wire carries them — degrees times ten million — and
formatted at the edge to four decimal places, about eleven metres. No floating
point is involved, which keeps it exact and costs nothing on a chip with no
FPU to spare. Four places is deliberate: enough for "a drone is over there",
not enough to imply a survey.

**Both transports decode, with one parser.** That is a property of the format
rather than a convenience: over BLE the payload is the service data under UUID
0xFFFA, and over Wi-Fi it is a vendor-specific element whose body is the ASTM
OUI followed by a vendor type of `0x0D` — the same byte, in the same position
relative to everything after it. The sniffer hands the element over from the
vendor type onward, which is where the BLE service data begins, and the rest is
identical.

A position beats a name: a drone's beacon SSID is usually its model, which the
label already says, so the SSID is the fallback when no position was broadcast
or the element was malformed — never a replacement for one.

### Hearing other nodes, without becoming findable

Another Observore in range can warn this one about what it has seen. **This
half only listens.** The device already scans BLE continuously, so hearing a
neighbour costs nothing and adds no exposure — which is the whole reason
receiving comes first.

Transmitting is a separate, opt-in change, and not built. A node that
broadcasts becomes findable by direction-finding, and *there is an observer
here* is the one thing a counter-surveillance device should not announce. So a
**receive-only node is a first-class configuration**, not a degraded one: it
benefits from every warning in range and emits nothing.

**Strangers may warn; only friends may silence.** A warning from a node this
device cannot authenticate can draw attention and can never quiet anything.
That asymmetry is what makes an open mesh survivable — an adversary standing up
ten invented nodes produces *noise*, never blindness, and noise is recoverable
in a way that silent suppression is not. In this slice it holds by
construction: there is no suppression path at all, and a warning contributes
**zero** to the score. The device says what it heard and whose it was, and a
person decides.

A warning rides in manufacturer-specific data under company `0xFFFF`, the SIG's
reserved non-production ID, with four magic bytes that make it specific —
`OBW1`. That is the same arrangement SquachWatch uses, because the company ID
alone means nothing: every hobby project made the same honest choice. There is
a test that adding ours did not shadow theirs.

It arrives as a `peer-detector`, which [#132](https://github.com/Forged-in-Feathers-Technology/observore/issues/132)
predicted against this project before any of it was built — *a meshing
Observore becomes a peer-detector in somebody else's device, including ours*.
What is new is that the finding says what the neighbour was warning about:

```
peer-detector   Observore node   warns drone 51.5074,-0.1278   -48 dBm
```

Four details of the format earn their place:

- **A sequence that must advance**, compared modulo the space rather than
  arithmetically. Repeating somebody's warning back at them forever is the
  cheapest attack on a table like this, and a plain greater-than would silence
  a node permanently the first time its counter wrapped or it rebooted.
- **One entry per node.** A node repeating itself must not be able to fill the
  table and push other nodes out.
- **Warnings expire**, after five minutes. A warning is about *now*; one kept
  for ever would let a single sighting look like a standing alarm.
- **A version we do not know is refused.** Unlike another project's format this
  one is ours, and reading fields by a layout we have since changed is how a
  position ends up in the wrong place.

A tag field is parsed for its length and then ignored, because there is no key
management yet. A tag that cannot be checked must never be mistaken for one
that has been, so `trusted` is always false — which is also why nothing in this
slice can be trusted enough to suppress.

**Every field is as small as it is for one reason.** A legacy advert gives 24
usable bytes once a flags element is present. The first version of the format
spent 23 of them on a positioned warning — it fit, with one byte to spare, and
left no room at all for a signature. Signed *and* positioned came to 27, which
would have needed BLE 5 extended advertising.

That is not a tolerable place to end up, and the reason is specific: the
classic ESP32 in both Cheap Yellow Displays is **BLE 4.2 and cannot *receive*
extended advertising**. Moving the format there would not merely stop those
boards transmitting — it would make them deaf to the mesh.

The alternative was to buy a Bluetooth SIG company identifier, which retires
the four magic bytes. That is **$1,250** for four bytes. So the fields were
made honest instead:

| field | was | now | why |
|---|---|---|---|
| node id | 4 | **2** | eight nodes in BLE range makes 16 bits ample |
| age | 2 | **1** | warnings expire at 300 s, so >255 described one already dropped |
| position | 4+4 | **3+3** | coarse was the stated preference anyway |

Twelve bytes bare, eighteen with a position, **twenty-two signed and
positioned** — two spare in a legacy advert, so every board here can hear a
signed warning that says where.

Positions go on the wire as degrees × 10⁷ divided by 256, in three signed
bytes: about 2.8 metres, against the eleven metres the four decimal places
shown to a person already imply. The divisor is a power of two, so the
arithmetic is exact both ways and there is no rounding decision to get wrong.

The version bumped to 2 when this changed. An unknown version is refused, so an
old node and a new one simply do not hear each other — the right failure, and
free right now because nothing transmits yet.

`/api/status` reports `peers: {nodes, warnings}`, which is how a headless
receive-only node says whether it is hearing anything at all.

### Choosing which monitors run

Not everybody wants every class. Somebody watching a car park for trackers has
no use for fleet telematics; somebody auditing a building's cameras does not
want a follower class at all. The console's **Monitors** panel switches each
one off, and the setting survives a reboot.

**Off means "keep seeing, stop reporting".** A disabled class is still
classified, still tracked, and still counted — it contributes nothing to the
score, stays out of the findings, and is not notified. That costs almost
nothing, because the device table is shared either way, and it means switching
a monitor back on shows what has been around *all along* rather than starting a
blank history. It is the same shape as the census: gather first, act later.

**A device with monitors off never reports `clear` as though it had looked.**
This matters more here than anywhere else in the device. An ignore rule leaves
a trail — the rule is listed, it carries what it suppressed, and a fingerprint
rule that covers a population retires itself. A switched-off monitor leaves
*nothing* to find afterwards. So the count travels with the verdict everywhere
the verdict goes:

- on the glass, in the coloured band itself, beside the level
- in the console, appended to the level: `clear — 2 monitors off`
- in `/api/status`, as `monitors: {off, off_mask}`
- at the top of every notification body, `(2 monitors off)` — sharing one
  note with the census when it has quieted something too
- in the log at every boot, naming each one

The notification note **leads** the body rather than trailing it, which is what
makes it impossible to lose. Reserving room for a trailing note was the first
attempt, and it was not good enough — whether the note survived depended on
where the last finding happened to land, and the test written to prove the
reservation worked passed just as happily with the reservation removed. Written
first there is no arithmetic to get wrong: a findings line is dropped instead,
and `+N more` already accounts for that.

**A protected class can be switched off, but only by a person.** Body camera,
ALPR and tracker are the classes this device exists to find, and an explicit
visible choice by the owner is different in kind from a baseline sweeping
something up by accident — so the console allows it, behind a confirmation that
says what it means. What stays impossible is anything *automatic* doing it: not
a baseline, not the census, not a future mesh peer. That is enforced by
construction — nothing but the console calls the setter — rather than by a
flag, so the rule is kept by not adding callers. The census, which does now
act by itself, obeys the same line from the other side: it can quiet an
unidentified household device and can never quiet a protected class.

**`unknown` is not a monitor** and cannot be switched off. It is what the
device says when nothing matched, so hiding it would mean hiding everything the
device could not name, which is the opposite of the point. The unclassified
list is untouched by this feature for the same reason.

**The stored value is which monitors are *off*, not which are on.** Classes get
appended to the list as the project learns to spot new things — fifteen so far.
Storing the on-set would mean a setting saved today has a zero where tomorrow's
class will be, and that class would arrive switched off on every device that had
ever saved a preference. Storing the off-set makes the default fall the safe
way: an unknown bit is zero, zero means on, and a missing or unreadable setting
means everything is on rather than nothing.

### Learning the furniture, then acting on it carefully

A baseline is a decision you make once, by hand, about a room you happen to be
standing in. The thing it is trying to approximate is *what is always around* —
and a stationary device is in a position to learn that by itself, over days,
without being asked ([#132](https://github.com/Forged-in-Feathers-Technology/observore/issues/132)).
The device keeps a census of what it keeps seeing, and now takes weight off the
score for it — but it shipped as **learn and report, suppress nothing** and
stayed that way for four days first.

That split was deliberate rather than unfinished, and it paid for itself. Twice
this project has silenced the thing it exists to notice — a baseline that
blinded the device outright, and a baseline that quieted a Flipper Zero by name
for a week — and both were judgements that had never been watched before they
were trusted. So membership was earned, persisted and reported first, against a
table that had time to be wrong in public. What came back off that table
changed the rule that was going to be written: see below.

**Membership is counted in days, not sightings.** Each known device carries a
bitmap of the days it has been seen on, one bit per day, and counts as
household at **three distinct days** inside a **sixteen-day** window. A
doorbell seen four hundred times this evening has one day to its name. A
visitor's phone seen three days running is household the day they leave, still
household a week later, and forgotten a fortnight after that — decay needs no
code, because the day falls out of the window and the bit goes with it.

Conflating "furniture" and "here right now" is the mistake behind every
revision of the follower class, which is why the rule is shaped this way: a
device at your elbow for three hours is interesting *precisely because* it is
not furniture.

Three things about the implementation are worth knowing, because each is a
place it could have been quietly wrong:

- **An unset clock produces no day at all.** The device detects from the moment
  it powers on and only learns the time when it next reaches a network, so
  early sightings genuinely have no date. They are dropped. Counting them
  against day zero would hand membership to whatever happened to be in the
  room during the first minute after every boot, which is the opposite of
  earning it over days.
- **The clock jumping is normal, not an edge case.** When SNTP answers, the day
  number goes from nothing to about nine thousand. Shifting a sixteen-bit mask
  by nine thousand is undefined behaviour in C rather than a convenient zero,
  so the distance is bounded before it is used, and a gap wider than the
  window clears the mask. A clock corrected *backwards* leaves the window
  alone rather than rewriting history it cannot reconstruct.
- **A date the record cannot hold is refused, not truncated.** The day number
  is sixteen bits, which runs out in 2179. A truncated day is not a near miss:
  it silently claims a different date, and the window would then be measured
  from it. This was found by a test asking about day 100000 and getting a
  confident wrong answer.

**Identity is the part most worth arguing with.** A device is keyed on its
advert fingerprint where it has one, because that hashes the *shape* of the
advert rather than the address, and the household's own phones rotate their
addresses every fifteen minutes — keyed on the address they would never reach a
second day, which is the case the census most wants to handle. Failing that,
a fixed address. A random address with no stable advert shape is skipped
entirely: it has no identity to remember, and noting it would fill the table
with single-day entries that can never become furniture, evicting the furniture
to do it.

The weakness in that is known and is the reason the learning shipped first. A
fingerprint identifies a *kind* of device, so two identical handsets share one,
and the mute store already has to retire fingerprint rules that turn out to
cover more than eight addresses. The census is measured against the same eight,
and what that measurement found is why it dampens rather than silences at the
ceiling.

It did not take long to happen. The first bench sweep on real air listed
twelve tracked slots carrying five distinct identities: one fingerprint
appeared three times in a single pass and another twice, which is a device
rotating its address across several slots — exactly what the fingerprint is
*for* — but it is indistinguishable, from here, from two identical devices in
one room. That is the measurement the suppression slice needs and the reason
it is not in this one.

Sixty-four devices are tracked, in NVS, and a sighting is only written when it
is a device's first of the day — a doorbell seen constantly would otherwise
wear the flash out recording a fact that stopped changing at breakfast. The
sweep runs **once a minute** over what is currently tracked rather than on
every sighting, which means a device has to still be there when the sweep comes
round. That is a second rule arriving by accident, so it is stated rather than
left implicit: a minute of presence, not a single frame, earns a device its day.

**It is on the device's own system page**, two lines, no browser and no
password:

```
 census   28 known, 6 household
 addrs    19x1  3x2-7  6x8+
 at cap   2 of 6 household
```

The second line is the measurement this whole slice exists to produce, and the
third is the one that decides what may be done with it. An identity at the
ceiling is *ambiguous*; an identity that is household is one the census would
be entitled to quiet. Where those overlap, quieting it could silence a whole
class of device — possibly the class this exists to notice — and neither the
spread nor the household count shows that overlap on its own.
Identities under a single address mean a fingerprint names a device; identities
at the eight-address ceiling mean it names a population, and quieting one would
quiet the lot. Without a clock the line says so rather than showing zeroes that
look like a quiet room.

**The first real reading changed the plan.** Four days on one board gave
`19x1 3x2-7 6x8+` — better than a fifth of identities at the ceiling, which is
not a rounding error and lands exactly on the limit the mute store already
uses to retire a fingerprint rule for describing a kind of device rather than
one. The identities at the ceiling are almost certainly the rotating ones, the
household's own phones: simultaneously what the census was built to quiet and
what cannot safely be quieted by fingerprint. Suppression keyed on an advert
shape is therefore safe only for the cases that never needed it.

That is the measurement earning its keep. The slice shipped as learn-but-do-not-act
specifically so this could be found out from data rather than discovered after
something had been silenced.

**And the overlap turned out to be total.** The figure that decides it is how
many of the ceiling identities have become household, which needs a clock and
so is reported on the first sweep that has one:

```
census: 21 known -- addresses 12x1 3x2-7 6x8+
census: 21 known, 10 household
census: 6 of 6 identities at the 8-address ceiling are household
```

Six of ten household identities are ambiguous, and **every ambiguous identity
is household**. There is no safe subset: the identities the census most wants
to quiet are exactly the ones whose advert shape may name a kind of device
rather than one.

So suppression keyed on a fingerprint was not built. What shipped instead is
two rules, and one that outranks both:

- **household and below the ceiling** — one device, identifiable, safe to
  quiet. Four of ten here: the doorbell, the printer, the things that do not
  rotate. It contributes no points and does not appear in the findings.
- **household and at the ceiling** — down-weighted, never silenced. It keeps
  **half** its points, rounded up so a single point never rounds to silence,
  and stays in the findings. If the shape really is one rotating phone the
  noise goes away; if it covers a population, a stranger's device still
  registers. The failure mode becomes under-alarmed rather than blind, which
  matters on a device that has been blinded twice before.
- **a protected class, whatever else is true** — reported, always. A body
  camera, a tracker, a drone, a Flipper, something that came with you: no
  amount of being around every day earns the right to stop saying so. This is
  checked *before* membership rather than after, so no later change to the
  rule can reach a protected class by another path.

A halved device is halved **before** the follower ceiling is applied, not
after, so a dampened follower spends half as much of that class's budget as
well. Halving the total afterwards would instead let twice as many of them
fill it, which is the opposite of down-weighting.

**None of it happens without a clock.** The day the census judges against is
handed to the scoring by the sweep, and until the first sweep that has a date
the day is unset and nothing is quieted at all. A board that cannot tell one
day from another cannot know what is furniture, and it withdraws the same way:
if the day goes away, the suppression goes with it.

**And it announces itself.** The verdict band carries the count of quieted
devices beside the level, for the same reason it carries the count of
switched-off monitors and a stronger one — a quieted device contributes nothing
*and* is absent from the findings, so without that number there is nothing
anywhere on the screen to say it was ever seen:

```
  OBSERVORE  CLEAR     score 0
  5 devices   3 quiet
```

Dampened devices are not in that count, because they are visible by being
listed. The exact pair is on the system page, in the present tense — not how
many identities are household, which is a fact about the stored table, but how
many devices in front of the radio right now are having weight taken off:

```
 quieted  3 silent, 2 at half
```

A notification carries it too, leading the body where it cannot be lost, and
alongside the monitors note rather than as a second parenthetical:

```
(2 monitors off, 1 quieted as household)
```

It was reachable only through the web console at first, which needs the network
*and* the console password — and that password is printed when the SoftAP comes
up and nowhere else. So a board with a screen and no browser to hand held this
and had no way to say it: the same shape of fault as a brightness setting you
cannot reach from a screen too dim to read.

`/api/status` reports it as `census: {known, household, days, quieted,
dampened}`. The first pair is what the table has learned — `known` climbing
while `household` stays at zero would mean the rule is never being satisfied,
and there is no other way to see that from outside. The last pair is what is
being done about it right now, which is the only thing that explains the
score.

### Counting the addresses, which is the number that decides the next step

The console grows a **Household census** panel listing what the device has
learned: each identity, how many of the required days it has, whether it counts
as household, and **how many distinct addresses it has been seen under**. The
same thing is on `/api/census`, which like the rest of the detection data needs
the console password.

The address count is the column that matters, and it is there because of a
limitation rather than a feature. An identity seen under many addresses is
*ambiguous*: either one device rotating its address — which is precisely what
keying on the advert's shape is for — or several identical devices sharing that
shape. **Nothing here can tell those apart**, and no amount of further
cleverness on one device will. What the count can say is how often the
ambiguous case arises at all, and that is the fact which decides whether
suppression keyed on a fingerprint is viable. The mute store already retires a
fingerprint rule that covers more than eight addresses for exactly this reason;
the census is measured against the same eight so the two compare directly.

It is a true count of distinct addresses, not a count of changes — up to eight,
after which it saturates and says so with a `+`. At the limit, "eight" and
"eight and still arriving" mean different things about a device, and the
difference is lost by a bare count. The addresses are stored as sixteen-bit
hashes, because the question is how many rather than which, and they do **not**
decay with the day window: a device that rotated through eight addresses a
fortnight ago really has been seen under eight. Membership is a claim about
now; this is a claim about what the identity is.

**What a full table gives up, and why it was wrong.** The table holds 64
identities. When it filled, the entry with the oldest day went — on the
reasoning that anything genuinely around every day is among the most recently
seen. The reasoning is sound; the implementation of it had a hole.

On a stationary board everything in range is seen again every day, so every
entry carries today's date, no entry is older than any other, and a scan for
the smallest date returns the *first slot every time* — whatever is in it. One
slot becomes a revolving door while the other sixty-three never move, and what
is in that slot may be two days into becoming furniture while fifty-four
single-day strangers sit untouched beside it. The cost is bounded — one entry,
not the table — but it is paid by the same entry every run, so that identity
can never establish itself.

The date was never the thing worth ranking on. How much an entry has shown is:

1. **Not household before household.** The table exists to remember furniture;
   evicting furniture to make room for a stranger is the one move that defeats
   the whole structure.
2. **Fewer distinct days before more.** A single-day entry has shown nothing
   yet; a two-day entry is one evening away.
3. **Older before newer** — the original rule, kept as the tie-break it should
   always have been.

Household-ness is measured against the day being asked about rather than the
stored mask, so an identity whose three days have fallen out of the window is
not furniture any more and its place is exactly the one that should go.

The test for this was written twice. The first version passed under both
rules, because the scenario it built had entries with mixed dates — which is
the case the old rule handles correctly. The one that bites needs every entry
to carry the same date and *different* numbers of days behind it, which is an
ordinary morning on a board that does not move.

**A full table says so.** The bench board sat at `64 known, 2 household` for
days and nothing distinguished a census that had stopped learning from one
that had finished. The count now reads `64 known (full)` on the screen and in
the boot log, and `/api/census` reports `max` and `full`.

**A census saved by v0.13.0 is discarded on upgrade**, because the stored entry
grew to hold the address set. The blob now carries a header saying which code
wrote it, and the reason is worth stating: the old entry was eight bytes and
this one is larger, so an old table with the right number of entries divides
evenly into the new entry size and would have restored as a smaller number of
plausible-looking nonsense. That is the same trap the touch calibration fell
into, and the answer is the same — record the provenance rather than try to
recognise the shape. The device says so and rebuilds over the following days.

## The screen

The ESP32-2432S028R — the 2.8" "Cheap Yellow Display" — is the one board here
with a panel, and the firmware draws on it: the level as a coloured band with
the score and device count, the top findings one per line in the same words a
digest uses, and uptime with the version along the bottom. It is a status
screen, redrawn every couple of seconds, and only the lines that changed are
sent.

Two decisions about what the screen is. Nothing on it is gated: the glass on a
desk is a personal display and the authentication belongs to the network-facing
console, not to the thing in the room with you. And the console password is
never drawn on it, for the same reason in reverse — a screen faces a room, and
the device already prints the password to serial for whoever is setting it up.
Touch does not change either rule. Someone who can press the glass is already
standing in front of it.

### Touch

The board carries an XPT2046 resistive controller on its own SPI pins, and
with `CONFIG_OBSERVORE_TOUCH` the bottom of the screen becomes a bar of three
buttons: **page**, **baseline** and **light**.

**page** moves between the watch page above and a system page — version,
board, uptime, what has been seen, free memory, the console's address, what
the last version check found, and how long the previous runs lasted with how
each one ended.

The address is there because on a board like this there is nowhere else to
read it: the device prints it to serial at boot and nothing else shows it, so
a console you cannot find is a console you do not have. The password is a
different matter and is still never drawn. **clear ignores** is on the system page, beside the update actions, and asks
twice like the baseline that usually created them. It exists because undoing
a baseline previously needed a console, and the board that most needed it —
a screen in a pocket with no network configured — had none: getting out of an
accidental baseline meant flashing a one-shot firmware twice. A device that
can silence a room with one tap should be able to unsilence it from the same
glass. The label carries the count, because "clear ignores" with nothing to
clear should not look like the same words hiding fifty-one rules.

**baseline** asks twice — the first tap shows `tap again to baseline`, and it
forgets after five seconds. A baseline is the most destructive thing this
device does, silencing everything in range at once, and it was the one path
with no guard: dismissing a single finding already took two taps and the
console already asks for confirmation. It was hit twice by accident in one day
on a board with no case, the second time while leaving an office, which
silenced the population there and invalidated the journey the board was being
carried on. It does what holding the button
does, and says so on the screen; the work happens in the main loop, which can
be most of a minute away inside a scan. **light** steps the backlight through
four levels and remembers the choice, because a 2.8" panel at full brightness
is a beacon in a dark room and a detector that comes back from a power cut at
full brightness at three in the morning has told the room something.

**Stacks are a budget too, and the only way to size one is to read it back.**
Both the drawing task and the touch task report their remaining stack whenever
the mark moves, the way the heap watch does. That exists because the touch
task was trimmed to 1,536 bytes by eye during a memory fix and overflowed the
moment `CONFIG_OBSERVORE_TOUCH_LOG_RAW` was switched on — formatting one log
line costs about a kilobyte of stack — so the procedure documented above for
calibrating a panel panicked the device on every touch. Measured, drawing a
page of keyboard leaves about a kilobyte spare at 3,072 bytes and a logged
touch about 1,200.

**Memory is the budget that governs this board.** It has no PSRAM, and during
an uplink window the console's scratch, the screen's buffers and the TLS
handshake behind an update check all want internal RAM at once. v0.8.2 shipped
with about twelve kilobytes less of it than v0.8.1 and the CYD could no longer
check for its own updates: the handshake failed, the check reported that it
could not connect, and since installing requires a successful check first, a
device in that state could not be updated over the air at all. v0.8.3 gave the
memory back — smaller task stacks, a shorter screen snapshot, and half the
console scratch — and the failure log now carries the free heap, because
"cannot connect" on its own points at the network rather than at the real
cause. Anything added to a display build should be measured against the free
heap reported during an uplink window, not during patrol, where there is
twenty kilobytes more of it and nothing looks wrong.

A figure to measure against, from a 2.8" CYD in the field running v0.11.0: an
update check takes the internal low-water from **26,580 bytes to 12,936**, with
the largest free block falling to 12,800. The check succeeds there, and the
margin it succeeds by is about thirteen kilobytes. Patrol on the same board
reports 48 KB free throughout and says nothing about any of this.

Free heap is not the whole story either. A later build had 31 KB free and
still could not check for updates, because the certificate check wanted a
single contiguous block of 4,437 bytes for an RSA signature and the heap was
too broken up to offer one. The device says so plainly — `Dynamic Impl:
alloc(4437 bytes) failed`, then `mbedtls_ssl_handshake returned -0x3000` — and
the answer was to stop holding the largest block for windows nobody uses: the
console's scratch is now taken on the first request of a window rather than
when the server starts. A console nobody opens costs nothing. A check that
falls due while somebody is reading the console may fail and retry in a later
window, which is the right way round.

Drawing moved to a task of its own when touch arrived. The main loop spends
about thirty seconds of every patrol cycle inside a blocking scan, and a
screen that only redrew there would ignore a finger for half a minute; now the
loop publishes a status and the drawing task renders it on its own clock.

### Joining a network from the screen

The network page lists what the last patrol scan saw — no scan is started for
it, because the chip has one radio and a scan on demand fights the sweep — and
tapping one opens a keyboard.

**The password is typed blind.** The key you press is shown, because a
keyboard that does not say what it registered is unusable on a resistive
panel, but the field itself shows dots. A **show** button reveals what has
been typed for fifteen seconds and then hides it again, turning the field
amber while it is legible. That is the same rule as everywhere else on this
screen: it faces a room, so what appears on it is a deliberate choice rather
than a default. The password is wiped from memory as soon as it is saved or
abandoned.

An open network needs no keyboard at all, and joining takes effect at the next
uplink window.

### Acting on what the screen shows

Two actions at the foot of the system page: **check for updates**, which asks
straight away rather than waiting for the daily check, and **install**, which
appears only when a check has found something. Install asks a second time
before it starts, because it stops the detector for minutes and then reboots
it — the same confirmation the console's button uses, for the same reason.

On the watch page, **touching a finding offers to ignore it**. The row turns
amber and asks; a second tap writes the rule. It is the device's broadcast
name where it has one and its address otherwise — deliberately not its advert
fingerprint, which identifies a kind of device rather than an individual, so
muting your own tracker that way would silence a stranger's. A baseline is
allowed that trade for a follower on a rotating address because it is a
statement about a whole room; one tap on one row is not.

Both confirmations lapse after five seconds and are cancelled by leaving the
page, so a press nobody meant — a resistive panel under a sleeve, a board
face-down on a desk — does nothing.

### The pocket watch face

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

### Brightness that follows the room

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

### Brightness without touch

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

### Calibrating a panel

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

## Notifications

Observore pushes to **[Gotify](https://gotify.net/)**, **[ntfy](https://ntfy.sh/)**
or **[Pushover](https://pushover.net/)**. Choose one in the console's
**Notifications** panel, or:

```
GET  /api/notify
POST /api/notify?provider=gotify&url=https://gotify.example.com&token=APP_TOKEN
POST /api/notify?provider=ntfy&url=https://ntfy.sh/your-topic[&token=TOKEN]
POST /api/notify?provider=pushover&token=APP_TOKEN&user=USER_KEY
POST /api/notify?test=1
POST /api/notify?clear=1
```

| Provider | URL | Credentials |
|---|---|---|
| Gotify | your server; `/message` is appended | application token |
| ntfy | the full topic URL, e.g. `https://ntfy.sh/your-topic` | optional bearer token |
| Pushover | none — it has one endpoint | application token **and** user key |

Urgency is chosen by what was found — a body camera or licence-plate reader is
urgent, a follower or tracker is high, a drone or smart glasses normal, a
camera low, and translated into each service's own scale. Pushover's urgent
maps to *high* rather than *emergency*: emergency requires retry and expire
parameters and keeps re-alerting until a human acknowledges, which is not a
reasonable default for a device that can see a police car drive past.

Escalations of the overall threat level are pushed too; drops are not, because
an alert that clears is not news.

Tokens and user keys are stored in NVS and are **write-only** — no endpoint
returns them, exactly like the Wi-Fi password. Saving with the token left blank
keeps the stored one, so changing a URL does not cost you a credential — but
only for the same provider. Switching provider with nothing entered starts
clean, because a token issued for one service must never be sent to another,
and the console says which happened.

**Sending needs the uplink.** Detections happen during patrol, which has no
network, so notices are queued and flushed the next time you are joined. The
queue holds 24 and drops the oldest when full: a detector that stops noticing
new things because its outbox is full would be worse than one that loses the
oldest notice. A failed send stays queued and is retried.

**Everything queued arrives as one message**, with the findings listed in order
of how much they warrant attention: body cameras and ALPR first, then followers,
trackers, smart glasses and drones, then fleet telematics and cameras. Within a
class the closest is listed first. The title carries the level and a census, as
in `alert: 6 findings (1 drone, 5 followers)`, and the whole message takes the
highest urgency present, so one body camera still arrives as urgent even when it
is listed behind quieter entries. Six lines fit; beyond that the message ends
with `+N more` rather than quietly losing the tail.

That ordering reflects how alarming a class is given how common it is, rather
than how surveillance-like it sounds, which is why cameras come last — a Ring
prefix is a doorbell far more often than it is aimed at you.

One message rather than one per finding is also what keeps the radio affordable.
Each send is a TLS handshake with certificate verification, and six of those
back to back in one thirty-second window drove the free internal heap to **184
bytes** on a C5 — eight bytes above the level that once cost 10,019 consecutive
delivery failures. Sending a single ranked list instead leaves that low-water
mark at about 16 KB.

The console shows sent, queued, failed and dropped counts, plus the last
transport error — `401`/`403` is reported as a bad token or key rather than as
a bare number.

### How far each provider has been tested

| Provider | Status |
|---|---|
| Gotify | **sent end to end** from the device to a live server over TLS |
| ntfy | the exact request the firmware builds was **accepted by ntfy.sh**, with the title, multi-line body and priority arriving intact |
| Pushover | the request shape was **accepted by api.pushover.net**, which parsed the form body and rejected only the deliberately invalid token — delivery itself is **unverified**, since that needs an account |
| Webhook | **sent end to end** from the device to a listener on another VLAN, over plain HTTP: bearer header, JSON content type and body all arrived as built |
| Telegram | the request is built to the Bot API's documented shape and host tested, including the bot token in the path and `disable_notification` for low urgency — delivery itself is **unverified**, since that needs a bot |

### Webhook

A JSON POST to whatever URL you give it, unchanged, with `Authorization:
Bearer <token>` when a token is set. The body is:

```json
{"source":"observore","urgency":"high","title":"alert: 2 findings","message":"..."}
```

`urgency` is one of `low`, `normal`, `high`, `urgent`. The body names its own
fields rather than pretending to be any one service's shape: a Home Assistant
webhook trigger reads it directly, and anything that wants Discord's `content`
or Slack's `text` maps it in a line of template. A body that tried to be all of
them at once would carry the message three times and not fit.

This is the one provider that can skip TLS. Pointing it at `http://` on your
own network costs no handshake at all, which on this device is the entire
expense of sending a notification. A Gotify or ntfy on the LAN over `http://`
gets the same saving.

#### Home Assistant

Home Assistant accepts webhooks with no authentication beyond the id itself,
which makes it the simplest thing to point Observore at and also means the id
is a secret: make it long and random. Create an automation with a webhook
trigger, either in the UI or as YAML:

```yaml
alias: Observore findings
triggers:
  - trigger: webhook
    webhook_id: observore-7f3a9c2e51b8d4
    allowed_methods:
      - POST
    local_only: true
actions:
  - action: notify.mobile_app_your_phone
    data:
      title: "{{ trigger.json.title }}"
      message: "{{ trigger.json.message }}"
      data:
        priority: "{{ 'high' if trigger.json.urgency in ['high', 'urgent'] else 'normal' }}"
```

`local_only` refuses the webhook from outside your network, which is where the
device is anyway. Then in Observore's console choose **Webhook** and enter:

```
http://192.168.1.10:8123/api/webhook/observore-7f3a9c2e51b8d4
```

Use the address, not `homeassistant.local`: mDNS is link-local and often does
not cross from an IoT VLAN. Leave the token blank; Home Assistant ignores it.
Press **Send test** while the device is on the uplink and the automation
should fire once with the title `Observore test`.

The four fields are all available as `trigger.json.<name>`. `urgency` is
`low`, `normal`, `high` or `urgent`, so a condition on it can route a
`bodycam` digest to a different notifier than a quiet `follower` one, or
switch on a light, or anything else an automation can do — which is the
point of sending to Home Assistant rather than to a phone directly.

Expect a POST at most once every few minutes and only when there is something
to say. The device is off the network while patrolling, and findings are
combined into one message per uplink window.

#### Anything else that takes a POST

The same body works for **n8n** and **Node-RED** webhook nodes as-is, and for
**Apprise** through its API, which can then fan out to whatever it supports.
**Discord** and **Slack** want a specific field name — `content` and `text`
respectively — so put a small relay in between, or use one of the above to
forward. Observore does not send the message under three names at once
because the request would not fit.

### Telegram

Token is the bot token, the second credential is the chat id. The token forms
part of the request path, which is how the Bot API is shaped, so the notifier
logs only the host of whatever it is configured with — a webhook URL is often
the credential too, and serial logs end up in bug reports.

All three wire formats are pinned by host tests: URL construction including
trailing slashes, header names, body encoding, and the priority mapping. The
Pushover body is form-encoded, so an advertised device name containing `&`
cannot inject a field. There is a test for exactly that.

## Cutting a release

```bash
git tag v0.13.0 && git push origin v0.13.0
```

That is the whole manual part. The tag push builds every shipped target,
collects each one's parts at the offsets its own build chose, assembles the ESP
Web Tools manifest, and attaches the lot to a **draft** release. Creating that
draft itself if it does not already exist.

Then read the notes, edit them if you like, and publish. Publishing is what
deploys the flasher page.

The order matters and is easy to get backwards:

- **`gh release create --draft` does not push the tag:** the build never
  triggers, and the only clue is an `untagged-<hash>` URL on the release page.
  Push the tag; the workflow makes the draft.
- **Publishing before the build finishes deploys a broken page:** `pages.yml`
  triggers on `release: published`, while the release workflow is what uploads
  the binaries. Publish first and the site goes live pointing at assets that
  do not exist. Draft, then build, then publish.
- **The `github-pages` environment must allow the tag:** deployments are
  restricted by ref, and a release-triggered run has a tag ref rather than a
  branch one. A `tag: v*` policy is what lets it deploy at all; without it the
  deploy job fails before running a single step.

## Partition layout

```
nvs        data nvs    0x9000     24K
phy_init   data phy    0xf000      4K
otadata    data ota    0x10000     8K
ota_0      app  ota_0  0x20000  1984K
ota_1      app  ota_1  0x210000 1984K
```

**The table is OTA-shaped even though OTA is not implemented**, and that is
deliberate. The layout is the expensive part to change later: the browser
flasher writes the partition table, so a device moving to a different layout
would silently lose whatever moved. Settling the offsets while there are few
devices costs nothing and means it never has to happen again.

Two offsets are the actual commitment, and only two:

- **`nvs` at `0x9000`** — every stored setting: mute rules, Wi-Fi credentials,
  the notifier token, the generated console password and the detection
  history. It is where ESP-IDF's `partitions_singleapp_large.csv` put it, which
  is what the earliest firmware used, and it has never moved.
- **`ota_0` at `0x20000`:** the running application.

Everything else stays free. `ota_1` holds no state, so its size and position
can change later; even growing `ota_0` is non-destructive, because the running
app stays put and only the scratch slot shifts.

With no `factory` partition and a blank `otadata`, the bootloader reports
`No factory image, trying OTA 0` and boots `ota_0`. Upgrading an existing
device to this table keeps everything: verified on a C5 that came back with its
35 mute rules, its network and the same console password.

Sized for a 4 MB board, the smallest supported (the C6-DevKitM-1). The
reference C5 and C6 DevKitC-1 kits carry 8 MB and Waveshare's C5 kit 16 MB;
those leave the remainder unused, because an image built for more flash than
the chip has does not boot at all, while the reverse is harmless.

Slot occupancy today is **C5 74%, C6 69%, S3 56%**. The C5 is the tightest and
the fastest growing, so CI builds every target and would fail there rather than
in the field, exactly as it did when the old 1500K partition overflowed by
27 KB.

Those numbers were 83 / 78 / 64 before three deliberate reductions, which are
worth knowing about because they are the levers if it ever gets tight again:

| | saved | why it is safe |
|---|---|---|
| `-Os` instead of `-Og` | 125 KB | optimising for size rather than for debugging |
| Common CA roots, not the full set | 51 KB | 44 authorities including Let's Encrypt, which self-hosted notifier endpoints overwhelmingly use |
| Console served gzipped | 17 KB | 25 KB of HTML becomes 8 KB, and the page also arrives faster |

Two further levers exist and were deliberately not pulled. Silencing assertion
messages saves another 63 KB but throws away the text that says what failed,
which is the wrong trade on a device where an unexplained restart used to be
indistinguishable from a quiet night. Raising the flash floor to 8 MB would
double every slot, at the cost of no longer booting on a 4 MB board.

**OTA itself is deliberately not implemented.** With no image signing and an
unencrypted console, an update path reachable over the network turns "somebody
on your LAN has the console password" into "somebody owns this device
permanently", a real escalation on a device meant to detect surveillance. That
waits on HTTPS and a decision about signing. Reserving the layout keeps the
option open at its cheapest moment without opening the hole.

## A note on internal RAM

The ESP32-S3 has about 180 KB of DRAM regardless of how much PSRAM is fitted,
and Wi-Fi and lwip allocate from it. Observore therefore builds its JSON responses
in **PSRAM** where there is any, not in static internal buffers.

Boards without PSRAM take a smaller budget from internal memory instead and
report fewer devices per request. The console says which it got, and the
heartbeat shows the consequence.

Whether a board has PSRAM is not a property of the chip. The C6 has none; the
C5 has it on `R`-suffixed modules and not otherwise, which is why the shipped
C5 image is built to boot either way. The XIAO ESP32S3 has 8 MB.

### When it ran low, not just how low

The console header shows free internal RAM and the lowest it has been since
boot, with when that happened and what the device was doing at the time. The
same is on `/api/status` under `heap`, and `/api/heap` lists the last eight
drops in order.

That is there because the low-water mark on its own is a puzzle. Two overnight
runs each came back with a number and nothing else: 1,072 bytes one night,
2,932 the next, with the moment it happened logged to a serial port that nobody
was reading. A minimum says the device nearly ran out; the moment, the mode and
the notification queue depth say why. A drop only counts once it is 2 KB past
the last one recorded, so a slow slide leaves a handful of milestones rather
than filling the list with noise.

**The mode distinguishes an update from the window it happened in.** A TLS
session for a version check or a download only ever runs inside an uplink
window, so every one of its dips used to record as `uplink` —
indistinguishable from the ordinary pressure of being associated. On the 2.8"
CYD those are not close: the board holds around 22 KB through a window and the
handshake takes it to between one and three kilobytes. It now records as
`update`:

```
I (311764) observore.update: up to date on v0.13.0-26-g901b14b
W (312774) internal heap low-water fell to 1156 bytes (update, 0 queued, largest block 11776)
```

It had to be a latch rather than a flag, and the bench is what said so. The
first attempt exported "is a TLS session open right now", which read **false
every time**: the handshake blocks the main loop, so by the time execution
returns to the heap sampler the session is closed and the dip it was meant to
explain has already happened. The check finished at 312023 ms and the record
was written at 313033, still labelled `uplink`. So the update marks that a
session ran and the first sample afterwards consumes the mark — which is
accurate rather than convenient, because `heap_caps_get_minimum_free_size()`
is a running minimum that captured the dip while the loop was blocked.

Three runs of the same check gave 1,468, 2,568 and 1,156 bytes. That the
handshake's floor moves by more than a kilobyte between runs is itself only
visible once the dips can be told apart.

### How long the last run lasted

The header also says how long the previous run lasted and how it ended, and
hovering it lists the eight before that. The device writes its uptime to flash
every five minutes; at boot, the last value written is the length of the run
that just ended, filed with the reason this boot happened. A run shorter than
five minutes leaves nothing behind, which is right: that was a false start,
not a run.

This is the battery question answered from measurement, with one precondition
that matters more than it looks. A device found up for seven hours after a
night on battery used to say only that; the run before it was gone. Now the
runs before it say whether its life is getting worse.

**A run is a power session, not a battery session.** The record measures from
boot to reset, and nothing in it distinguishes time on USB from time on a
cell. So a run that ended in `power-on` or `brownout` is the battery's life
only when the *whole* run was on battery — the device was unplugged before it
booted, or at least immediately after.

That is easy to get wrong, and this project got it wrong in writing before
getting it wrong in practice: the round board recorded a single run of
**21 h 44 min**, of which about 4 h 25 min was on the cell and the rest was
plugged in. Five times the real figure, reported with complete confidence. If
a run began on USB, the only honest reading is to subtract by hand from when
you know it was unplugged.

**So each run now records what the cell did across it** ([#165](https://github.com/Forged-in-Feathers-Technology/observore/issues/165)).
The first voltage reading of a run is kept as its start and never replaced;
every later one replaces its end. That is enough on its own:

| span | what it means |
|---|---|
| 4.20 V → 3.20 V | a battery run that went the distance |
| 4.20 V → 4.20 V | plugged in throughout — its length says nothing about the battery |
| 4.20 V → 3.90 V | a short battery run, or a long one on a tiring cell |

It also shows **how deeply each run discharged**, which is what says whether a
cell is getting worse rather than only how long it lasted this time.

**No transition detection**, deliberately. Watching for a sustained decline to
infer "now on battery" is flakier than it sounds: a full cell sitting on USB
looks much like a full cell on battery, and a weak USB supply that cannot hold
the cell up looks exactly like being unplugged — which this project has
already met, on a laptop port that could not supply charge current and the
Wi-Fi radio's turn-on surge together.

**Only the round AMOLED board can do this**, because it is the only board that
can measure its own supply. The CYDs and the devkit have their usable ADC pins
taken by the display and touch, established by probing. Those boards record
zero, which the console and the screen read as "not measured" and show
nothing — not a flat cell.

The record's format version went to 2 for the two new fields, and the old
records are **carried forward** rather than discarded: their lengths and
endings are still true and their voltages simply were not recorded. The census
discarded its old format on a similar change and was right to, because there
an old blob divided evenly into the new entry size and would have restored as
garbage; the danger was misreading, not upgrading. Here the version byte is
checked before either layout is read, and the runs being carried over are
measurements somebody made by leaving a board on a battery overnight.

The same list is on `/api/status` under `runs`, newest first, with `mv_start`
and `mv_end`.

**Measured: 25.1 hours.** An ESP32-C5 devkit on a 2,000 mAh cell, running
v0.8.3 untouched from a full charge until the cell's protection cut off —
`90311 s, ended by power-on`. That figure survives the correction above
because the run really was battery from end to end. That is with the radio at full duty: no light
sleep, a passive dual-band scan and a channel sweep every two minutes, and the
Bluetooth scanner running the whole time. Roughly 80 mA average, so a pack's
capacity in milliamp-hours divided by eighty is a fair first guess at hours for
any other cell.

**Measured: about 4 h 25 min** on the round AMOLED board's 400 mAh cell,
timed by hand from when it was unplugged to when it died, because its run
record spanned USB time as well. That is roughly **90 mA** — only about ten
more than the headless devkit, for a board carrying a lit dial, an
accelerometer, a hardware clock and eight megabytes of PSRAM.

Ten milliamps for a watch face is the clearest vindication the dial's design
has had. Black is genuinely off on an AMOLED, the face is mostly black, and
the hour markers and hands are a few hundred lit pixels out of 217,000.

**Measured: about 19.5 hours** on the 3.5" CYD's 3,000 mAh cell, which is
roughly **154 mA**. That run needs no correction, unlike the two above: it
began on battery, ran until the cell died, and so the record means what it
says.

Which gives three figures and one conclusion:

| board | screen | draw |
|---|---|---|
| ESP32-C5 devkit | none | ~80 mA |
| Waveshare 1.43" AMOLED | 466x466, mostly-black dial | ~90 mA |
| ESP32-3248S035R | 480x320 backlit LCD | ~154 mA |

**A screen costs either ten milliamps or seventy-four, depending on the
panel.** An AMOLED lights the pixels it needs and a dark watch face needs
almost none; a backlit LCD illuminates the whole panel whatever is on it. The
same information, on the same sized glass, for seven times the power.

Two caveats on the 154 mA, both of which make it a pessimistic figure. The
cell's 3,000 mAh is its rating rather than a measurement. And that board was
sitting at **full brightness** throughout, because its stored level had been
forced there while chasing an unrelated fault and never put back — on a board
where the backlight is three quarters of the draw, the brightness setting is
not a detail. It has a photoresistor and an automatic mode for exactly this
reason.

So the brightness setting remains the one lever worth pulling before a long
day out, and on a backlit board it is worth more than everything else
combined.

The number took three attempts to get honestly. The first two runs ended
because the firmware was reflashed mid-run, and a third gave six hours because
the build had a debug task sampling the heap a hundred times a second, which
never let the chip idle — visible in the record as a six-hour run among
twenty-five-hour ones, which is exactly what the record is for.

The flash cost is stated because it was checked: about three NVS entries per
write fills a page every few hours, which is a sector erase every day or so per
page against a rating of a hundred thousand — a lifetime measured in centuries,
so this is not the thing that wears out.

### A tab in the background is quiet

The console polls only while it can be seen. A tab left open in the background
used to reconnect every uplink window, ask for everything, and stop reading,
and six stalled responses sitting in the network stack's send buffers is how a
browser tab once took the device down. A hidden tab now sends nothing, which
also means the device is not held on the uplink by a page nobody is looking at,
and gets back to detecting. Switching back to the tab refreshes it at once.

This is not premature tuning. An earlier version used static internal scratch
(two 20 KB device snapshots plus 32 KB and 12 KB response buffers) and drove
free internal heap down to 1.4 KB with a largest free block of 768 bytes. At
that point the SoftAP still beaconed and still accepted associations, but could
no longer allocate a buffer to answer an ARP request — the console loaded once
after boot and then went dead, looking exactly like a network fault. The
heartbeat now reports free, minimum-ever and largest-block internal heap for
this reason; if the console ever goes quiet again, read that line first.

```
clear | score 0 | 0 devices | 227 sightings | 0/0 frames | heap 103687 free, 42568 min, 31744 largest
```

## What survives a restart

Mute rules, credentials and the console password always persisted. What the
device had actually *seen* did not, so a power blip or a firmware update erased
the whole picture, and an unexplained reboot was indistinguishable from a
quiet night.

Classified detections now survive, in the **History** panel and at
`/api/history`. Rows carry the times they were seen and are marked *earlier*
when they predate the current boot.

What is deliberately **not** kept is the live device table. That is working
state — 192 slots of mostly unidentified churn, rewritten constantly, and
persisting it would cost far more flash than it is worth. Only things that
matched a signature are recorded, which is also what keeps the write rate low
enough to be safe.

**Flash wear is the whole design constraint**, so the write policy is worth
stating plainly:

- A **new** classification is worth a write, and is rate limited to one every
  five minutes rather than written immediately.
- **Another sighting** of something already recorded moves a counter and a
  timestamp, and never triggers a write on its own. It rides along with the
  next one.
- Opening the console forces a write, because somebody is about to read it.
- Writes are **held back while the clock is unset**, for up to five minutes.
  Dating is retroactive only while the monotonic timestamps live, and those die
  with the boot: an entry written before the time is known, on a device that
  then restarts, is undatable for ever. After the grace period an undated
  record still beats no record, so it is written anyway.

The effect is that the write rate tracks how many genuinely new things the
device sees, which in a baselined deployment is close to zero.

On-device history is a convenience, not the record. If detections matter, give
the device an uplink and a notifier. Those leave the device as they happen.

## Knowing which firmware it is running

The console header shows the running version and the board it was built for,
taken from the application descriptor that ESP-IDF stamps at build time and
from the board identifier compiled into the image. A build cut from a tag reports
that tag, `v0.5.0`; a build made after one reports what `git describe` says,
`v0.5.0-3-gce8e56e`, which is a different thing and says so.

**The boot banner says the same thing over the serial cable**, which is the
one route that needs neither the network nor the console password:

```
I (2341) observore: Observore v0.12.0-2-gc139518-dirty starting -- board devkit-esp32c5, booted from ota_0
```

That is a real line from a bench build, which is why it reads
`v0.12.0-2-gc139518-dirty` rather than a tag: two commits past `v0.12.0` with
uncommitted changes in the tree. A board flashed from a release says `v0.12.0`
and nothing else.

It says only `Observore starting` in builds before this, and the gap was
noticed the usual way: a board on the desk with a cable already attached to
it, and the quickest answer to *what is on this thing* turning out to be
reading the application descriptor back out of flash with `esptool`.

The partition is there for the same reason the version is. Images built on
ESP-IDF v5.5 hung between the PSRAM memory test and user code when booted
from `ota_1`, so every over-the-air update rolled back and none ever took —
and a device that has quietly fallen back to its previous slot looks exactly
like one that was never updated. `booted from ota_0` after an update that
reported success is the symptom, and nothing else on the device says it out
loud.

It also checks, once a day by default, whether a newer release has been
published. The document it reads is `firmware/boards.json` — the same file the
web flasher uses, so anything installable is by definition visible to the
device, and there is no second piece of infrastructure to keep in step. The
check runs inside an uplink window the device was going to open anyway, and it
runs *after* the notification queue has been sent, so a findings digest never
waits behind a version check for memory or for airtime.

**Check now** in the console's Firmware panel asks for that check straight
away, in the window the console is already holding open; the answer appears in
the same panel a few seconds later, with when it was last checked. It exists
because three releases in a row were noticed only after a reboot: the daily
check had already run that morning, and the release came out in the afternoon.
A check that fails to connect is tried once more five seconds later before it
says so, since on the bench the first attempt a few seconds after the uplink
came up did exactly that and the second did not. `POST /api/update/check` is the
same thing without the button.

A newer version is mentioned once, on the end of the next digest, and shown in
the console until it is installed. Once per version, not once per window: a
device that repeats itself is one people stop reading.

The comparison refuses to guess. A version it cannot parse means no update,
in either direction — the failure mode of a wrong yes is a device downgrading
itself, so an unreadable answer is treated as no answer. A build made after a
tag is ahead of that tag, so a device running `v0.5.0-3-gce8e56e` is not
offered `v0.5.0` as an upgrade.

The board matters as much as the version. Three of the nine boards here are
ESP32-C5s and three are the same classic ESP32 behind different glass, and none
of those images are interchangeable, which is why the installer offers a picker
rather than deciding from the chip. A device updating itself has the same
problem and nobody to ask, so it carries the answer: `xiao-esp32s3`,
`devkit-esp32c5`, `xiao-esp32c5`, `xiao-esp32c6`, `cyd-2432s028r-st7789`,
`cyd-2432s028r-ili9341`, `cyd-3248s035r-st7796`, `nm-cyd-c5` or
`waveshare-s3-amoled-143`. CI asserts that each board's build resolves to its own
name, checked against the `sdkconfig` the build actually produced rather than
by re-deriving the layering.

Nothing is downloaded by the check. Installing is a separate and deliberate
act: the console grows an **Install update** button when there is something to
install, and it asks for confirmation, because the device stops detecting for
the few minutes the download takes and then restarts.

### What installing actually does

The image comes from the release asset host rather than from beside the
manifest. That is not arbitrary. The flasher site sits behind a CDN that
compresses this file and then answers range requests against the compressed
copy while serving the decompressed one: a request for the first 4096 bytes
comes back with 10,602, and the reported total is 906,227 against a real size
of 1,447,536. An image assembled from those offsets is not the image.

The Bluetooth stack is shut down for the duration, which frees about **24 KB**
of internal memory. That is not a nicety. The hardware AES driver needs
DMA-capable internal memory that `MBEDTLS_EXTERNAL_MEM_ALLOC` cannot move to
PSRAM, and with the controller resident the TLS handshake fails with
`esp-aes: Failed to allocate memory` before a single byte is downloaded.
Nothing is lost by stopping it: the sniffer is already suspended on the uplink,
so a device downloading firmware is not detecting anything either way.

Stopping it is one way, for that boot. A finished update reboots, and a failed
one reboots too, so there is no resume path to get wrong on the one code path
that only runs when something has already gone wrong.

The image is checked before it is trusted. Its descriptor is read from the
first chunk, before anything reaches flash, and rejected unless it is the same
project and a newer version. It is written to whichever OTA slot is not
running, so the image that is working is never the one being overwritten.

### Rollback

A new image gets one chance. The bootloader arms a watchdog before handing
over, and the image has to confirm itself or the next reset puts the old one
back. Confirmation means completing a patrol cycle: booted, radio up, scanned,
swept, returned. An image that panics or hangs never gets there.

It deliberately does **not** wait for the network. Reaching the uplink is a
fact about the network rather than about the image, and an access point that is
down for five minutes would otherwise revert a perfectly good update. The
timing also rules it out: the watchdog window is capped at 120 seconds and the
first uplink is a patrol window away. Confirmation is measured at about 66
seconds after boot, which leaves roughly 54 seconds of margin.

This was all verified on hardware by stamping a build as an older version and
letting a real device find, download and install the published release. It
booted the new image from the second OTA slot, and because that release predates
the confirmation code it never confirmed, so the watchdog reset it and the
bootloader restored the previous image. Rollback is not theoretical here.

## Knowing what time it is

A detector that cannot say *when* has given you half an answer. The device
learns the time by SNTP during an uplink window and reports UTC everywhere; the
console renders it in the browser's own zone, because the device has no idea
what zone it is in and guessing would be worse than not.

**DHCP first, then a configured server.** This is the important part. Observore
is meant to sit on the segment the cameras are on, and that segment is
routinely firewalled from the internet. The same isolation that stops a push
notification reaching its server stops `pool.ntp.org` answering. A router's own
NTP is reachable from inside that fence. `OBSERVORE_NTP_SERVER` is the fallback
for networks that hand out no NTP option.

**Timestamps are retroactive.** The conversion works from the monotonic clock,
which runs from boot, so a sighting recorded *before* the time was known is
still dated correctly once it is. That matters because the device starts
detecting the instant it powers on and only learns the time when it next
reaches the network — often a few seconds later, sometimes never.

**Unknown is reported, not faked.** Until the clock is set, `time_valid` is
false, the absolute fields are empty, and the console and the system page both
say `clock not set`. Only the relative "4m ago" is shown. Emitting 1970 dressed
up as a timestamp would be worse than admitting the clock is unset, on a device
whose output is meant to be evidence.

### The clock says what set it

`observore_clock_valid()` used to be one comparison: is the system clock past
2025? That conflated two different claims — "the clock holds a plausible
number" and "the device knows what time it is" — and anything that seeded the
clock would have satisfied the first while the second stayed false.

That became load-bearing when the census started suppressing things. The
census counts *separate days*, so a clock that never advances records every
sighting on one day and nothing ever becomes furniture; a clock set to the
wrong day records furniture against a date it will not be judged against.
Neither failure announces itself.

So the clock carries a **source**, and `valid` needs both halves — something
actually set it, *and* the result is a plausible date:

| source | what it is |
|---|---|
| `none` | never set: the time is whatever boot left behind |
| `person` | handed in from a browser at the console |
| `chip` | read from the board's own RTC at startup |
| `network` | SNTP, which is the one that is actually right |

The order is a ranking, and a better source overwrites a worse one without
being asked — which is how a person's rough answer gets quietly corrected the
moment the network arrives. A worse source cannot undo a better one, because a
browser tab left open on the console offers to set the clock on every reload
and must not be able to replace an SNTP answer with its own opinion.

**The source lives in RTC memory, with the clock it describes.** System time
survives `esp_restart` — which here means an update installing itself, or a
panic or watchdog — because ESP-IDF keeps the boot time in RTC slow memory. On
the bench it survived an EN-pin reset as well: a watch reset over USB came back
holding both the time and `network` as its source, and correctly declined its
own RTC chip's answer in favour of the better one it already had. It does not
survive a power cycle. A source kept in an ordinary static would be lost on a restart
while the clock it describes survived, turning a known time into an unknown
one across every OTA. A source kept in NVS would do the opposite and outlive
the clock, claiming a synced time on a board that has just been plugged in.
That memory has exactly the right lifetime, which is the whole reason for
using it rather than either. It is read through a magic word, because it is
not initialised at power-on and would otherwise report whichever source the
previous occupant's bits happened to spell.

**One door to the census.** `observore_clock_day()` is the only thing that
turns the clock into a census day, and it returns "no day" whenever the source
is `none`. `observore_census_day()` stays pure arithmetic that converts
whatever timestamp it is handed — the host tests drive it directly — and
deciding whether that timestamp is worth believing is a question for the clock.
Asking it at four separate call sites is how three of them end up still asking
the old way.

### Setting the clock from the browser

Most of these boards have no RTC chip, and the places this device is worth
carrying are routinely places where no NTP server is reachable: a camera VLAN
with no route out, a field with no uplink at all. Those boards cold boot into
1970 and stay there — findings with no date, and a census that cannot count a
single day.

The browser on the other end of the console already knows the time to the
millisecond. `POST /api/time?epoch=<seconds>` hands it over, and the console
shows a **set the clock from this browser** button whenever the time did not
come from the network. It is a worse clock than SNTP and a far better one than
none, and it is recorded as what it is rather than passed off as a sync.

It is a *source*, not an override:

- **A time outside the window a running device can be in is refused, not
  clamped.** The floor is 2025-01-01, which keeps an unsynced ESP's 1970 out.
  There is a ceiling at 2100 too, because a time set by hand can be wrong in
  the other direction — a browser with its year typed wrong is no more usable
  than 1970, and a date past what the census day number can hold would be
  stored as a different date entirely. A device that will not use a time is
  honest; one that silently moves it to the nearest allowed instant has
  invented a date and will timestamp evidence with it.
- **It goes straight into the RTC chip where there is one**, so a time handed
  in by a person survives the power cycle that loses it everywhere else.
- **It is behind the console password**, like everything else there. Not
  because the time is secret — because a clock now decides what the census
  suppresses, and an unauthenticated endpoint that moves the date is an
  unauthenticated endpoint that decides what the device stops reporting.
- **The two refusals say different things.** A time outside the window is
  yours to correct; a clock already set from somewhere better is not a problem
  at all. The first version answered both with one message and could not say
  which had happened, which the bench demonstrated within a minute.

The decision itself is a pure function, `observore_clock_rule()`, and the host
tests drive it. The rest of the clock needs a real `settimeofday()` and a chip
on an I2C bus, which is exactly how a rule like this ends up never exercised.
One of the cases it pins down is the half that is easy to leave out: a recorded
source whose clock reads 1970 must not outrank a real answer, or a device would
refuse every source forever and could never be dated again.

**Notifications carry the time the thing was seen**, not the time the message
was sent. Notices are queued while patrolling and flushed on the next uplink,
so delivery can trail detection by twenty minutes, and the queue exists
precisely for the case where that gap is longest.

Both forms are reported: `last_seen_s` counts seconds ago, `last_seen` is
ISO-8601 UTC. A client with no clock of its own still needs the first.
`/api/status` also reports `clock`, which is the source name, and the system
page on the screen shows the time with the source beside it — `not set` where
there is none, and where to set it.

### Where the internal RAM actually went

With the figures honest, the next question was answerable: what is using it?
`idf.py size-components` says `libmain.a` holds 62 KB of DRAM — more than the
Wi-Fi stack, the Bluetooth controller and lwIP put together. That is this
firmware's own static allocation, and it is worth being careful about, because
the first reading of it was wrong twice over.

`nm` reported `OBSERVORE_VENDOR_OUIS` at 41 KB with section type `d`, which
looks like 41 KB of vendor name strings sitting in RAM. Its address is
`0x3f420058`, which is flash-mapped DROM — exactly where a `const` table
belongs. The section label says what kind of symbol it is; only the address
says where it lives. And `size-components`' DRAM column for an archive
includes flash rodata, so 62 KB was never 62 KB of RAM either.

Filtering by address range instead gives the real picture: **98.6 KB** of
static symbols in internal DRAM, and one of them is a quarter of it.

| symbol | bytes | what |
|---|---|---|
| `s_devices` | **26,112** | the device table, 192 slots at 136 bytes |
| `s_rules` | 6,144 | mute rules |
| `s_hist` | 3,456 | detection history |
| `rows` (console) | 3,456 | one page of history, for the console |
| `records` (Wi-Fi) | 2,944 | the access-point scan |
| `s_store` | 1,796 | the household census |

Same class of mistake as the heap caps, caught the same way: check what the
number is measuring before believing it.

### The device table is per board now

The tracker holds 192 devices, and the table is the largest single static
allocation this firmware makes. That is affordable on most boards and was not
affordable on one: the 3.5" Cheap Yellow Display drives a 480x320 panel with
no PSRAM to hold a framebuffer, runs Wi-Fi and BLE together, and was sitting
at a low-water mark of **820 bytes**. It took an unexplained `SW_CPU_RESET`
during a bench sweep and could not serve its own status page.

`OBSERVORE_MAX_DEVICES` is a Kconfig option, set to 96 on that profile alone:

| | before | after |
|---|---|---|
| low-water | **820** | **5,008** |
| largest free block | 4,864 | 12,288 |
| console body buffer | stepped down to 2–3 KB | the full 4 KB |
| `/api/status` | 503, then transport failures | answers |

Thirteen kilobytes back, and the board can report on itself for the first
time. The cost is stated rather than hidden: it tracks half as many devices at
once, and the table evicts the device heard from longest ago, so in a crowded
place it holds a shorter window of the room. Mute rules, the census and the
detection history are separate and untouched.

**Then the same setting on three more profiles**, once they had been measured
properly. The 2.8" boards and the NM-CYD-C5 were left at 192 on the strength
of one reading apiece — 11,452 bytes on the 2.8" — which was precisely the
thin evidence this exercise was about avoiding. The low-water figure is a
running *minimum*, so a high one means only that nothing had dipped yet
during however long you happened to watch. Ten minutes of watching gave
**1,588** and **2,544**.

| board | before | after |
|---|---|---|
| NM-CYD-C5 | 2,544 | **14,140** |
| 2.8" CYD, resting free heap | 11,064 | **23,764** |
| 2.8" CYD, low-water | 1,588 | 1,468 |

The last row is not a failure of the change and is worth reading carefully.
The 2.8" board's low-water before its first update check was **20,444**; what
takes it to 1,468 is the TLS handshake behind that check, which succeeds:

```
I (311022) esp-x509-crt-bundle: Certificate validated
I (312422) observore.update: up to date on v0.13.0-23-gb059acb-dirty
W (313432) internal heap low-water fell to 1468 bytes (largest block 11776)
```

So the table gave that board thirteen kilobytes back for the whole time it is
not shaking hands with GitHub, and its remaining exposure is a two-second peak
once per check rather than its baseline. That peak is a separate problem with
a separate fix, and naming it is better than letting a single number stand for
both.

The NM-CYD-C5 is the one that makes the point about where the table lives: it
has PSRAM and was still tight, because the table is a **static array**. PSRAM
takes the console's scratch and the framebuffer, both allocated at runtime, and
cannot take a symbol the linker has already placed in internal RAM.

**What this does not claim.** A listener held the serial port for the panic
text and the crash did not recur, so the `SW_CPU_RESET` remains unexplained.
What is fixed is the margin, which was not survivable; whether it was the
cause of that particular reset is not established. The device table being
tunable is also what makes the next such measurement cheap.

### The heap figure was measuring the wrong pool

Every number this device reported about its own memory — the boot log, the
uplink line, `/api/status`, the heap-watch ring — was taken with
`MALLOC_CAP_INTERNAL`. On the classic ESP32 that includes regions which are
**32-bit access only**: IRAM that no `malloc()` will hand out for ordinary
data. So the "largest free block" being compared against allocations that
failed was, in part, a block that could never have satisfied them.

It was found from the other end. On the 3.5" CYD the console could log you in
but `/api/status` answered `503 not enough memory right now` for an entire
uplink window — minute after minute — while the device reported 16,164 bytes
free with a 10,240-byte largest block and the buffer it could not get was
4,096 bytes. Those two statements cannot both be true.

Measured against `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, which is what every
allocation in this firmware actually draws from:

| | reported before | actually available |
|---|---|---|
| free | 16,164 | **5,324** |
| low-water | 11,896 | **932** |
| largest free block | 10,240 | **4,608** |

The console's internal fallback asks for 2,880 bytes of device snapshot and a
4,096-byte JSON buffer. The first fits in a 4,608-byte block and leaves about
1,700 behind; the second cannot be had. That is the 503, exactly, and it was
invisible for as long as the figures were three times too high.

The caps are now named once, in `observore_heapwatch.h`, and mean "internal
RAM that can hold data". A number that cannot be compared against the
allocation it is being used to explain is worse than no number, because it
sends the search somewhere else — which it has now done at least twice here.

**Two things follow from the honest numbers, and neither is fixed yet:** the
console's internal budget is sized for memory this board does not have, and a
932-byte low-water mark on a board that is merely sitting in an uplink window
is far too close to nothing. Both need tuning against figures that can be
trusted, which is why they are not being tuned in the same change that started
trusting them.

## Notifications, TLS and memory

A device left on a battery overnight came back having logged **10,019 failed
notifications and zero successes** in seven hours. Two separate faults, found
only because it ran for a night rather than a minute.

**TLS could not allocate.** Every failure was
`mbedtls_ssl_setup returned -0x7F00`, which is `MBEDTLS_ERR_SSL_ALLOC_FAILED`.
No packet was ever sent, so nothing appeared in the network logs, and the
obvious suspicion of a firewall was wrong. mbedTLS wanted one 16 KB contiguous
allocation and the largest free block was reported as 15,360 bytes. Short by a
kilobyte, every time.

**That figure was measured against the wrong pool** — see below. It was
`MALLOC_CAP_INTERNAL`, which on the classic ESP32 includes 32-bit-only IRAM
that cannot hold data, so the block mbedTLS could actually have had was
smaller than 15,360 and the shortfall was larger than a kilobyte. The
diagnosis and the three fixes were right; the number quoted alongside them was
flattering.

Internal RAM is genuinely oversubscribed on these parts: Wi-Fi, BLE, lwip and
the console share roughly 160 KB. Three changes together:

| | |
|---|---|
| Inbound TLS record buffer 16 KB → 8 KB | still holds any realistic certificate chain |
| Dynamic TLS buffers | allocated while in use, not for the life of the connection |
| mbedTLS allocates from PSRAM | on boards that have it — 8 MB was sitting unused |

Minimum free internal heap across four TLS pushes went from **336 bytes to
10,372**. The C6 has no PSRAM and keeps the default allocator, so TLS there
remains tight; the first two changes still apply.

**The retries had no backoff.** The pump ran from the main loop and a failure
simply returned, so the next pass tried again immediately — one attempt every
2.6 seconds, for as long as the device was associated, forever. That is a TLS
setup each time, radio time in the narrow associated window, and battery, all
for an endpoint that was not going to answer.

Failures now back off from 30 seconds to a 15-minute ceiling and reset on
success. Notices stay queued throughout; this delays retries, it never
discards. `/api/notify` reports `retry_in_s`, so a notifier that has gone quiet
can say why.

The heartbeat also logs the moment the internal-heap low-water mark drops, with
the mode and queue depth. A watermark on its own says the device nearly died
and nothing about when or during what.

## What happens when the radio misbehaves

A scan that fails is not always transient, and the failure mode is nasty. One
`ESP_ERR_WIFI_TIMEOUT` once left the driver believing a scan was still running,
after which every channel change was refused and every subsequent scan returned
zero access points — **permanently**. Fifteen good scans, one timeout, then 560
empty ones. The device had stopped seeing Wi-Fi entirely while still looking
perfectly alive and reporting a quiet neighbourhood, which is the worst way for
a detector to fail.

Repeated scan failures now stop the stuck scan and, after three in a row,
restart the radio through the ordinary mode-change path. Verified by injecting
failures: three refusals, one restart, scanning back to 27 APs on the next
cycle.


A driver error on a mode change does not restart the device. It is logged, the
current mode is left in place, and the next cycle tries again:

```
W observore.wifi: wifi start failed: ESP_ERR_INVALID_STATE -- staying put and retrying
W observore: could not enter patrol (ESP_ERR_INVALID_STATE) -- retrying next cycle
I observore.wifi: patrol mode: scanning and sniffing      <- recovered, next cycle
```

This matters more here than the phrase "error handling" suggests. Patrol and
uplink alternate every couple of minutes for as long as the device is deployed,
so a transient failure on that path is not rare, and the device table, the
sighting counts and the follower heuristic all live in RAM. Aborting would
therefore not degrade the device, it would restart it, and throw away exactly
the accumulating evidence it exists to gather, silently.

Initialisation is still fatal. If the radio will not come up at all there is
nothing useful to continue doing, and a boot loop is at least honest about it.

## Limitations

Read these before trusting it.

- **MAC randomisation is followed, not defeated -- but only for BLE, and only
  by a kind, not an individual:** phones rotate their Bluetooth address every
  ~15 minutes. A new random address whose advert fingerprint matches a device
  that went quiet in the last 20 minutes, at a similar signal strength, is taken
  to be the same device, so a handset that stays two hours shows as one device
  that stayed two hours and rotated seven times, rather than eight strangers.
  The fingerprint identifies a *kind* of device, so two identical handsets can
  be merged if one falls silent as the other appears; the quiet requirement
  makes that rare, and the cost is a merged pair rather than a missed one.
  Wi-Fi addresses rotate too and are not yet followed.
- **Absence of evidence is not evidence of absence:** wired cameras, cellular
  ALPR units with the radio off, and — on anything but a C5. 5 GHz devices
  are invisible to it.
  A clear reading means nothing was detected, not that nothing is there.
- **Vendor prefixes identify manufacturers, not purpose:** a Ring OUI is a
  Ring device; it is a doorbell far more often than it is surveillance aimed
  at you. Camera-class hits are scored at 1 point for this reason.
- **Some signatures are inferred, not documented:** the Meta company IDs and
  several name keywords are derived from public reporting rather than vendor
  specification, and are unverified against hardware. Treat a
  `smart-glasses` hit as a lead.
- **Fast Pair is noisy:** ordinary headphones advertise `0xFE2C`. It is
  reported because Google's Find Hub trackers use it too.
- **2.4 GHz only, unless you have a C5:** on every chip but the ESP32-C5 the
  sniffer sweeps channels 1–13 and nothing above them. A C5 sweeps 5 GHz as
  well, but a slice per cycle rather than all of it at once, so a 5 GHz device
  takes longer to appear than a 2.4 GHz one.
- **WPS is frequently not broadcast at all:** the manufacturer and model
  reading is only as good as the access points near you, and many modern ones
  never send the element. A survey of 150 beacons in one flat found none. The
  parser is unit tested against malformed and hostile input, but its attribute
  decoding has not yet met a real WPS element on air, only synthetic ones.
- **Vendor lookup covers MA-L only:** the IEEE also issues smaller MA-M and
  MA-S blocks, which the generator does not read, so some genuinely assigned
  prefixes resolve to nothing. A miss is reported as unknown rather than
  guessed at.
- **Muting is a blunt instrument:** a `class` or `oui` rule will hide a real
  threat that happens to share a category or vendor with something you
  dismissed. Prefer `mac`, `name` or `fingerprint` rules where you can.
- **The console is authenticated but not encrypted:** it asks for the console
  password and then carries a session cookie, which stops casual and
  accidental access, but over plain HTTP that cookie can be read by anything
  sniffing the LAN, which on Wi-Fi is any device in range holding the
  passphrase. Authentication is not a substitute for encryption.
- **A device that is updating is not a device that is watching:** installing
  firmware stops the Bluetooth stack and holds the uplink for the few minutes
  the download takes, and the sniffer is already suspended there. That is
  several minutes of not detecting anything, which is why an update is never
  installed on the device's own initiative. It checks, it says so, and it waits
  to be asked.
- **A first update cannot arrive over the air:** an updater has to be running
  before it can fetch anything, so a device on a release older than v0.6.0 has
  to be flashed once over USB. After that it can update itself.
- **Notification delivery is verified for three providers of five:** Gotify
  and the webhook have been sent end to end from the device; ntfy accepted the
  exact request the firmware builds; Pushover and Telegram have had their
  request shapes checked but not delivery, since each needs an account.

## Legal note

Observore is a receiver. It observes broadcasts that are, by design, transmitted
publicly and unencrypted. It does not deauthenticate, inject, jam, associate,
crack, or interfere with anything. Passive reception of broadcast frames is
lawful in most jurisdictions, but "most" is not "all", and what you do with a
log is a separate question from how you gathered it. Check your local law.

## Credit

Built by [Forged in Feathers Technology](https://www.forgedinfeatherstechnology.com),
alongside [WarDeck](https://www.forgedinfeatherstechnology.com/wardeck) —
the same interest in what the radio spectrum around you is actually doing,
pointed in the opposite direction.

The concept — passive BLE plus Wi-Fi surveillance detection with a decaying
threat score on a pocket-sized ESP32. Comes from
[simeononsecurity/eye-spy](https://github.com/simeononsecurity/eye-spy)
(Apache-2.0). Observore is an independent implementation for different hardware:
ESP-IDF rather than Arduino, a web console rather than an RGB LED, and vendor
tables generated from the IEEE registry rather than maintained by hand. No code
was taken from it.

## Licence

MIT. See [LICENSE](LICENSE).
