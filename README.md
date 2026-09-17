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

Observer and omnivore: it eats surveillance signals.

## Getting started

You need a XIAO ESP32S3 or an ESP32-C5 board and a USB-C cable. The whole first
run takes about ten minutes, most of it waiting. The browser flasher reads which
chip you plugged in and installs the matching build, so there is nothing to
choose.

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

## What it does

**While patrolling, it listens and does not answer.** Nothing it observes can
observe it back, because nothing leaves the radio:

| | |
|---|---|
| BLE scan | passive — no `SCAN_REQ` is ever emitted |
| Wi-Fi access-point scan | **passive**, no probe requests |
| Wi-Fi sniff | receive-only |

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
  (vendor IE `FA:0B:BC`/`0x0D`).
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

Each hit adds points by class (bodycam and ALPR 5, follower 4, tracker/drone/
glasses 3, telematics 2, camera 1). The score decays one point per minute and
each device can only re-score every 120 seconds, so one loud beacon cannot run
it away while sustained presence keeps it lit.

- **0–2 clear** — LED winks once every 5 s (green)
- **3–5 caution** — LED pulses once a second (amber)
- **6+ alert** — LED flutters (red)

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
and cannot be inferred from it — the ESP32-C5 appears twice here, once as a
Waveshare kit with a WS2812 and a CH343 UART bridge, and once as a XIAO with a
plain LED and only native USB. Board files live in [`boards/`](boards/):

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
| `boards/cyd-2432s028r-st7789` | GPIO16, plain (green of the RGB) | GPIO0 | original ESP32, **no PSRAM, no notifier**; the ST7789 2.8" CYD, screen not driven yet |

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

A **fingerprint** hashes only the parts of an advert that survive rotation:
which AD fields are present and how long they are, the manufacturer's company
ID, the service UUIDs, and the local name. The variable payload is excluded, so
a Find My advert fingerprints identically before and after it rotates its key.
It is not perfectly stable — a device that varies its advert *structure* gets a
new fingerprint, but it holds for the large majority.

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

That board does not send notifications. It cannot: the notifier is a build
option (`CONFIG_OBSERVORE_NOTIFIER`) and the CYD profile leaves it out, which
is what makes the port fit. Every memory problem this project has had was on
the uplink and most of it was the TLS handshake behind a notification, so the
board with the least RAM and no PSRAM is better off without the code than with
it switched off. Off means absent: the console hides the panel, and asking to
configure a provider says "this build has no notifier". Any board can be built
that way; the CYD is the one that is.

The profile is named for the panel controller, `cyd-2432s028r-st7789`, not
just the product, because the same product ships with an ILI9341 in other
revisions, a build for one shows garbage on the other, and the chip is
identical so the flasher cannot tell them apart. Three things about this panel
were settled on the bench against what is written about it: it does not want
colour inversion, the SPI path does not byte-swap for you, and the panel driver
returns before DMA has read the buffer you handed it. Each is a build setting
or a comment where the next revision will look.

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
git tag v0.7.1 && git push origin v0.7.1
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

It also checks, once a day by default, whether a newer release has been
published. The document it reads is `firmware/boards.json` — the same file the
web flasher uses, so anything installable is by definition visible to the
device, and there is no second piece of infrastructure to keep in step. The
check runs inside an uplink window the device was going to open anyway, and it
runs *after* the notification queue has been sent, so a findings digest never
waits behind a version check for memory or for airtime.

A newer version is mentioned once, on the end of the next digest, and shown in
the console until it is installed. Once per version, not once per window: a
device that repeats itself is one people stop reading.

The comparison refuses to guess. A version it cannot parse means no update,
in either direction — the failure mode of a wrong yes is a device downgrading
itself, so an unreadable answer is treated as no answer. A build made after a
tag is ahead of that tag, so a device running `v0.5.0-3-gce8e56e` is not
offered `v0.5.0` as an upgrade.

The board matters as much as the version. Two of the four boards here are
ESP32-C5s and their images are not interchangeable, which is why the installer
offers a picker rather than deciding from the chip. A device updating itself has
the same problem and nobody to ask, so it carries the answer: `xiao-esp32s3`,
`devkit-esp32c5`, `xiao-esp32c5`, `xiao-esp32c6` or `cyd-2432s028r-st7789`. CI asserts that each board's
build resolves to its own name, checked against the `sdkconfig` the build
actually produced rather than by re-deriving the layering.

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
false, the absolute fields are empty, and the console says `clock not set`
next to the uptime. Only the relative "4m ago" is shown. Emitting 1970 dressed
up as a timestamp would be worse than admitting the clock is unset, on a device
whose output is meant to be evidence.

**Notifications carry the time the thing was seen**, not the time the message
was sent. Notices are queued while patrolling and flushed on the next uplink,
so delivery can trail detection by twenty minutes, and the queue exists
precisely for the case where that gap is longest.

Both forms are reported: `last_seen_s` counts seconds ago, `last_seen` is
ISO-8601 UTC. A client with no clock of its own still needs the first.

## Notifications, TLS and memory

A device left on a battery overnight came back having logged **10,019 failed
notifications and zero successes** in seven hours. Two separate faults, found
only because it ran for a night rather than a minute.

**TLS could not allocate.** Every failure was
`mbedtls_ssl_setup returned -0x7F00`, which is `MBEDTLS_ERR_SSL_ALLOC_FAILED`.
No packet was ever sent, so nothing appeared in the network logs, and the
obvious suspicion of a firewall was wrong. mbedTLS wanted one 16 KB contiguous
allocation and the largest free block was 15,360 bytes. Short by a kilobyte,
every time.

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
