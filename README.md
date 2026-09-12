# Observore

[![CI](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml/badge.svg)](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Forged-in-Feathers-Technology/observore?sort=semver)](https://github.com/Forged-in-Feathers-Technology/observore/releases)

A passive counter-surveillance detector for the [Seeed Studio XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
and the dual-band [ESP32-C5](https://www.espressif.com/en/products/socs/esp32-c5),
built by [Forged in Feathers Technology](https://www.forgedinfeatherstechnology.com).

It tells you what is watching you. Built to sit in one place and watch that
place: it learns what is normally there, then reports what is new. It listens
for the radio signatures of body cameras, licence-plate readers, IP cameras,
Bluetooth trackers, smart glasses and Remote ID drones, scores what it finds,
pushes notifications, and serves the log on your network.

Observer and carnivore: it eats surveillance signals.

## Getting started

You need a XIAO ESP32S3 or an ESP32-C5 board and a USB-C cable. The whole first
run takes about ten minutes, most of it waiting. The browser flasher reads which
chip you plugged in and installs the matching build, so there is nothing to
choose.

**The quickest route is the [browser
flasher](https://observore.forgedinfeatherstechnology.com/)** — any
Chromium-based desktop browser (Chrome, Edge, Opera, or Brave 1.69 and later),
no toolchain. Firefox and Safari do not implement Web Serial, and no mobile
browser does. It writes the firmware without touching the
stored configuration, so it upgrades an existing device without losing its mute
rules, credentials or console password. Skip to step 2 below once it finishes.

To build it yourself instead you also need
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5 or later.

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

You will need it in a moment, and serial is the only place it appears — see
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
answers for about thirty seconds out of every two and a half minutes — that is
[deliberate](#patrol-and-uplink-alternate).

**5. Optionally, set up notifications.** In the **Notifications** panel, put in
a [Gotify](https://gotify.net/) server URL and application token, Save, then
**Send test** while the uplink is up.

**6. Leave it where it will live, and take a baseline.** Let it patrol for ten
minutes or so, then open the console and press **Set baseline**. That marks
everything currently in range as known and resets the score, so from then on it
reports what is *new* rather than the whole neighbourhood.

**Then expect quiet.** After a good baseline Observore should say almost
nothing. A silent device is the normal state, not a broken one — the heartbeat
on the serial log tells you it is still watching.

## What it does

**While patrolling, it listens and does not answer.** Nothing it observes can
observe it back, because nothing leaves the radio:

| | |
|---|---|
| BLE scan | passive — no `SCAN_REQ` is ever emitted |
| Wi-Fi access-point scan | **passive** — no probe requests |
| Wi-Fi sniff | receive-only |

The Wi-Fi scan being passive matters as much as the BLE one. An active scan
broadcasts probe requests carrying the device's own MAC on every channel, every
cycle — and probe requests are exactly what presence analytics and Wi-Fi
tracking systems collect. A detector that announced itself to the things it
was built to notice would be self-defeating. The cost is dwell time, not
coverage: access points beacon around ten times a second, hidden ones included.

It does transmit in the other two modes, and there is no way around that:

- **Console** — the SoftAP beacons and serves the page.
- **Uplink** — it is associated to your network and pushes notifications.

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
  (vendor IE `FA:0B:BC`/`0x0D`).
- **Name and SSID keywords** — for hardware that announces itself.
- **Vendor labelling** — 10,348 benign vendor prefixes across 43 common
  manufacturers (Apple, Samsung, Ubiquiti, Espressif, Google, …), also
  generated from the IEEE registry. These **never classify and never score**.
  They exist so an anonymous MAC reads as "Apple" and you can recognise your
  own gear at a glance. Kept in a table entirely separate from the threat
  prefixes, because mixing "this is a surveillance camera" with "this is a
  Samsung" is how a detector starts crying wolf at its owner's phone.
- **Persistence** — the follower heuristic: an unclassified BLE address seen
  3+ times spanning 5+ minutes is reported as following you. This is the part
  that catches hardware with no signature at all, and it is the reason to
  build the thing.

### Scoring

Each hit adds points by class (bodycam and ALPR 5, follower 4, tracker/drone/
glasses 3, telematics 2, camera 1). The score decays one point per minute and
each device can only re-score every 120 seconds, so one loud beacon cannot run
it away while sustained presence keeps it lit.

- **0–2 clear** — LED winks once every 5 s (green)
- **3–5 caution** — LED pulses once a second (amber)
- **6+ alert** — LED flutters (red)

The rhythm is what the XIAO's single monochrome LED can say, and it is the same
on every board. Boards with an addressable WS2812 — the C5 kits — add the
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
round in five. The practical consequence is latency, not blindness — a 5 GHz
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
associated to an access point are mutually exclusive — so Observore cannot both
watch the band and serve you a web page at the same time.

- **Patrol** — unassociated, scanning and sniffing. No network.
- **Uplink** — joined to your own network. The console is on your LAN and
  notifications can be sent. Wi-Fi sniffing is suspended.
- **Console** — SoftAP, for first-time setup or when away from your network.

BLE scanning continues in all three; it is unaffected by the Wi-Fi channel, and
it is where most detections come from.

| Gesture | Effect |
|---|---|
| hold 1.5 s | swap between patrol and uplink |
| hold 4 s | raise the console SoftAP |

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
are never cut off mid-page — but only up to `OBSERVORE_UPLINK_MAX_S`. The
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
one — which asks for an *open* network, and a WPA2 access point refuses that
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

### Finding the device on your network

Observore offers the name **`observore`** (configurable as
`OBSERVORE_HOSTNAME`) two ways: over mDNS as `observore.local`, and as the
hostname in its DHCP request — which is what a router registers in its own DNS
and shows in its client list.

The DHCP one is the more useful across subnets, because ordinary DNS routes and
multicast does not.

Two caveats, both real:

- **mDNS is link-local multicast and does not route between subnets.** If
  Observore sits on an isolated VLAN and you browse from the main LAN, the
  `.local` name will not resolve unless your router reflects mDNS across both
  networks — on UniFi that is the *Multicast DNS* setting, and it must be
  enabled on each network, not just one.
- **A DNS domain ending in `.local` collides with mDNS.** RFC 6762 reserves
  `.local` for multicast, so most resolvers send `*.local` to mDNS and never
  ask your DNS server. If your LAN domain is something like `house.local`,
  names under it are ambiguous for every client, not just this one — a DHCP
  reservation plus a static record under a non-`.local` domain sidesteps the
  whole problem.
- **Some clients cannot resolve `.local` at all.** A Linux box with no Avahi
  and `systemd-resolved` showing `-mDNS` has no multicast resolver, so the name
  will fail there however the network is configured.
- **It is only on the network during its uplink window.** While patrolling it
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

## Building

For a first run, follow [Getting started](#getting-started) — this section is
the reference for everything after that.

### Flashing a release without a toolchain

Every tagged release carries `bootloader.bin`, `partition-table.bin`,
`observore.bin`, a `SHA256SUMS`, and an [ESP Web
Tools](https://esphome.github.io/esp-web-tools/) `manifest.json`. With
`esptool` alone:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
    0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 observore.bin
```

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

`idf.py set-target esp32s3` is needed once in a fresh checkout.

### Other targets

The reference board is the XIAO ESP32S3 and every measurement here was taken on
it. The firmware also builds for `esp32c5` and `esp32c6`, and CI builds all
three so portability breaks surface immediately rather than months later.

The **ESP32-C5 is the interesting one**, because it is dual-band: it is the only
supported chip that can see 5 GHz at all. On a C5 the sniffer sweeps both bands
(see [Channels](#channels)); on every other chip 5 GHz is simply invisible.

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

**On hardware verification:** the S3 is verified on real hardware continuously.
The C5 build is complete but its radio behaviour — 5 GHz capture, and the
BLE/Wi-Fi coexistence numbers below — has not yet been measured on a board.
Treat the coexistence table as S3 measurements until that happens. C6 is
compile-tested only.

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
pointless — the SoftAP's BSSID is in every Wi-Fi scan and the derivation would
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
for a session cookie, so the password is not repeated on every request — which
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
for every supported target, and a validation of the generated OUI table — that both tables
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
Measured here, all fourteen nearby BLE devices used rotating addresses — a
MAC-based baseline would have been worthless within the hour.

A **fingerprint** hashes only the parts of an advert that survive rotation:
which AD fields are present and how long they are, the manufacturer's company
ID, the service UUIDs, and the local name. The variable payload is excluded, so
a Find My advert fingerprints identically before and after it rotates its key.
It is not perfectly stable — a device that varies its advert *structure* gets a
new fingerprint — but it holds for the large majority.

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

It mutes detected threats too, which is the point: your own doorbell camera is
exactly the thing you want silenced. It asks for confirmation once, and
**Clear ignores** undoes all of it.

Baseline picks the most durable rule each device supports: its name if it
broadcasts one, else its advert fingerprint, else — only as a last resort — its
MAC. It reports the breakdown, and counts how many rules are merely temporary
because they had to fall back to a rotating address.

Measured on real air: fourteen devices in range, all of them rotating their
addresses, produced three name rules and ten fingerprint rules and **zero**
MAC rules. A minute later, after rotation, the score was still zero.

Up to 128 rules are stored, in NVS, surviving reboots.

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
camera low — and translated into each service's own scale. Pushover's urgent
maps to *high* rather than *emergency*: emergency requires retry and expire
parameters and keeps re-alerting until a human acknowledges, which is not a
reasonable default for a device that can see a police car drive past.

Escalations of the overall threat level are pushed too; drops are not, because
an alert that clears is not news.

Tokens and user keys are stored in NVS and are **write-only** — no endpoint
returns them, exactly like the Wi-Fi password.

**Sending needs the uplink.** Detections happen during patrol, which has no
network, so notices are queued and flushed the next time you are joined. The
queue holds 24 and drops the oldest when full: a detector that stops noticing
new things because its outbox is full would be worse than one that loses the
oldest notice. A failed send stays queued and is retried.

The console shows sent, queued, failed and dropped counts, plus the last
transport error — `401`/`403` is reported as a bad token or key rather than as
a bare number.

### How far each provider has been tested

| Provider | Status |
|---|---|
| Gotify | **sent end to end** from the device to a live server over TLS |
| ntfy | the exact request the firmware builds was **accepted by ntfy.sh**, with the title, multi-line body and priority arriving intact |
| Pushover | the request shape was **accepted by api.pushover.net**, which parsed the form body and rejected only the deliberately invalid token — delivery itself is **unverified**, since that needs an account |

All three wire formats are pinned by host tests: URL construction including
trailing slashes, header names, body encoding, and the priority mapping. The
Pushover body is form-encoded, so an advertised device name containing `&`
cannot inject a field — there is a test for exactly that.

## Partition layout

`partitions.csv` keeps `nvs` and `phy_init` at exactly the offsets ESP-IDF's
`partitions_singleapp_large` used, and grows only the application partition,
which is last. A device flashed with this table therefore keeps everything it
has stored — the mute rules, the Wi-Fi credentials, the notifier token and its
generated console password all live in `nvs` at `0x9000` and are untouched.

The old 1500 KB application partition was sized for the ESP32-S3, where the
firmware is about 1.24 MB. The same source built for a RISC-V target is roughly
a fifth larger — 1.49 MB on the C6 — which overflowed it by 27 KB and failed
the build outright. Rather than trimming features to fit, the table now claims
some of the flash that was sitting unused: it previously described 1.5 MB of an
8 MB part.

## A note on internal RAM

The ESP32-S3 has about 180 KB of DRAM regardless of how much PSRAM is fitted,
and Wi-Fi and lwip allocate from it. Observore therefore builds its JSON responses
in **PSRAM** where there is any, not in static internal buffers.

Targets without PSRAM — the C3, C5 and C6 — take a smaller budget from internal
memory instead and report fewer devices per request. The console says which it
got, and the heartbeat shows the consequence.

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

## Limitations

Read these before trusting it.

- **MAC randomisation defeats the follower heuristic.** Modern phones and
  most trackers in separated mode rotate their Bluetooth address every ~15
  minutes. A follower that rotates will never accumulate 3 sightings under one
  address. Observore catches devices with static or slowly-rotating addresses; it
  will miss a well-behaved rotating one.
- **Absence of evidence is not evidence of absence.** Wired cameras, cellular
  ALPR units with the radio off, and — on anything but a C5 — 5 GHz devices
  are invisible to it.
  A clear reading means nothing was detected, not that nothing is there.
- **Vendor prefixes identify manufacturers, not purpose.** A Ring OUI is a
  Ring device; it is a doorbell far more often than it is surveillance aimed
  at you. Camera-class hits are scored at 1 point for this reason.
- **Some signatures are inferred, not documented.** The Meta company IDs and
  several name keywords are derived from public reporting rather than vendor
  specification, and are unverified against hardware. Treat a
  `smart-glasses` hit as a lead.
- **Fast Pair is noisy.** Ordinary headphones advertise `0xFE2C`. It is
  reported because Google's Find Hub trackers use it too.
- **2.4 GHz only, unless you have a C5.** On every chip but the ESP32-C5 the
  sniffer sweeps channels 1–13 and nothing above them. A C5 sweeps 5 GHz as
  well, but a slice per cycle rather than all of it at once, so a 5 GHz device
  takes longer to appear than a 2.4 GHz one.
- **Vendor lookup covers MA-L only.** The IEEE also issues smaller MA-M and
  MA-S blocks, which the generator does not read, so some genuinely assigned
  prefixes resolve to nothing. A miss is reported as unknown rather than
  guessed at.
- **Muting is a blunt instrument.** A `class` or `oui` rule will hide a real
  threat that happens to share a category or vendor with something you
  dismissed. Prefer `mac`, `name` or `fingerprint` rules where you can.
- **The console is authenticated but not encrypted.** It asks for the console
  password and then carries a session cookie, which stops casual and
  accidental access — but over plain HTTP that cookie can be read by anything
  sniffing the LAN, which on Wi-Fi is any device in range holding the
  passphrase. Authentication is not a substitute for encryption.
- **Notification delivery is verified for two providers of three.** Gotify has
  been sent end to end from the device; ntfy accepted the exact request the
  firmware builds; Pushover accepted the request's shape but delivery has not
  been confirmed, since that needs an account.

## Legal note

Observore is a receiver. It observes broadcasts that are, by design, transmitted
publicly and unencrypted. It does not deauthenticate, inject, jam, associate,
crack, or interfere with anything. Passive reception of broadcast frames is
lawful in most jurisdictions — but "most" is not "all", and what you do with a
log is a separate question from how you gathered it. Check your local law.

## Credit

Built by [Forged in Feathers Technology](https://www.forgedinfeatherstechnology.com),
alongside [WarDeck](https://github.com/Forged-in-Feathers-Technology/wardeck) —
the same interest in what the radio spectrum around you is actually doing,
pointed in the opposite direction.

The concept — passive BLE plus Wi-Fi surveillance detection with a decaying
threat score on a pocket-sized ESP32 — comes from
[simeononsecurity/eye-spy](https://github.com/simeononsecurity/eye-spy)
(Apache-2.0). Observore is an independent implementation for different hardware:
ESP-IDF rather than Arduino, a web console rather than an RGB LED, and vendor
tables generated from the IEEE registry rather than maintained by hand. No code
was taken from it.

## Licence

MIT. See [LICENSE](LICENSE).
