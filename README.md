# Argus

A passive counter-surveillance detector for the [Seeed Studio XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/).

It tells you what is watching you. It listens for the radio signatures of body
cameras, licence-plate readers, IP cameras, Bluetooth trackers, smart glasses
and Remote ID drones, scores what it finds, and shows you the log on a web page
you raise on demand.

Named for Argus Panoptes, who had a hundred eyes and was set to watch.

## What it does

**It listens, and it does not answer.** BLE scanning is passive — the radio
never emits a `SCAN_REQ`, so nothing it observes can observe it back. Wi-Fi
sniffing is receive-only. The device transmits exactly once: when you hold the
button to raise the console, and only for as long as you leave it up.

Detection runs across three phases:

| Phase | What it catches |
|---|---|
| BLE passive scan (continuous) | trackers, body cameras, smart glasses, Remote ID drones, followers |
| Wi-Fi active scan (~3 s/cycle) | camera and ALPR vendor APs, camera-keyword SSIDs |
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

- **0–2 clear** — LED winks once every 5 s
- **3–5 caution** — LED pulses once a second
- **6+ alert** — LED flutters

Adverts weaker than −90 dBm are discarded; they are far enough away to be
someone else's problem and they dominate the false-positive rate.

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
a sniffer that works. Both values are tunable under `menuconfig` → **Argus**.

## Why there are modes

The ESP32-S3 has one radio on one channel. Channel-hopping to sniff and staying
associated to an access point are mutually exclusive — so Argus cannot both
watch the band and serve you a web page at the same time.

- **Patrol** (default) — unassociated, scanning and sniffing. No network.
- **Console** — SoftAP up, web UI served, sniffing suspended.
- **Uplink** — joined to your own network, web UI reachable on your LAN,
  sniffing suspended.

Hold the BOOT button for 1.5 s to cycle. Uplink is skipped when no network is
configured. BLE scanning continues in all three modes; it is unaffected by the
Wi-Fi channel, and it is where most detections come from.

## Joining your network

Uplink mode puts the console on your LAN, so you can read the log without the
SoftAP dance. **It costs detection**: while associated, the radio is pinned to
your AP's channel and the Wi-Fi sniffer is suspended. BLE scanning and AP scans
continue. If the network cannot be joined, Argus falls back to patrol rather
than sitting associated to nothing.

Credentials never belong in a tracked file. There are three ways to set them,
and the first is the one to prefer:

**1. At runtime, from the console.** Hold the button, join the SoftAP, and fill
in the **Network** panel. Stored in NVS. Nothing touches this repo at all, and
nothing needs rebuilding to re-point a device at a different network.

**2. `idf.py menuconfig`** → **Argus** → Wi-Fi uplink. This writes `sdkconfig`,
which is gitignored.

**3. A gitignored `credentials.conf`**, for reproducible or CI builds:

```bash
cp credentials.conf.example credentials.conf   # gitignored
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;credentials.conf" build
```

A credential in NVS wins over one compiled in, so setting it at runtime is not
silently reverted by reflashing the same firmware.

Set `ARGUS_WIFI_AUTOJOIN=n` to keep full patrol coverage and reach the uplink
only on demand via the button.

### Keeping credentials out of the repo

`CONFIG_ARGUS_WIFI_SSID` and `CONFIG_ARGUS_WIFI_PASSWORD` are empty in the
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

## Build and flash

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5 or later.

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The XIAO uses the S3's native USB-Serial/JTAG, so it enumerates as
`/dev/ttyACM0`, not `/dev/ttyUSB0`.

Configure the LED pin, button pin and console SoftAP credentials under
`idf.py menuconfig` → **Argus**. **Change the default console password.**

### Reading the log

Hold the button, join the `console-XXXXXX` network, open
<http://192.168.4.1/>. The page lists every classified device with its class,
MAC, signal, evidence and how long ago it was last heard.

### Tests

The classification, parsing and scoring logic builds and runs on the host with
no hardware and no ESP-IDF:

```bash
make -C test test
```

### Regenerating the OUI table

`main/argus_oui_table.h` is generated, not hand-maintained. Vendors get new
prefixes; refresh it with:

```bash
python3 tools/gen_oui_table.py
```

Both tables live in that one generated header. The threat categories are in
`CATEGORIES` and the benign vendor list is in `VENDORS` — add vendors there,
not to the generated header. A prefix claimed as a threat is never also listed
as benign.

Each benign prefix costs 4 bytes of flash (the name is an index into a shared
table, not a pointer per row). The 43 vendors shipped cost about 40 KB, taking
the firmware from 973 KB to 1032 KB — roughly 4% of the app partition. Adding
more is cheap; trimming `VENDORS` is the way to claw it back.

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

| Rule | Ignores | Use it for |
|---|---|---|
| `mac` | one exact address | your own phone, your own tag |
| `oui` | a whole vendor prefix | a neighbour's camera brand |
| `class` | an entire class | every `camera` on a busy street |
| `ssid` | an SSID substring, case-insensitive | a building's camera network |

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
way to add one.

## Limitations

Read these before trusting it.

- **MAC randomisation defeats the follower heuristic.** Modern phones and
  most trackers in separated mode rotate their Bluetooth address every ~15
  minutes. A follower that rotates will never accumulate 3 sightings under one
  address. Argus catches devices with static or slowly-rotating addresses; it
  will miss a well-behaved rotating one.
- **Absence of evidence is not evidence of absence.** Wired cameras, cellular
  ALPR units with the radio off, and anything on 5 GHz are invisible to it.
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
- **2.4 GHz only.** The sniffer sweeps channels 1–13.

## Legal note

Argus is a receiver. It observes broadcasts that are, by design, transmitted
publicly and unencrypted. It does not deauthenticate, inject, jam, associate,
crack, or interfere with anything. Passive reception of broadcast frames is
lawful in most jurisdictions — but "most" is not "all", and what you do with a
log is a separate question from how you gathered it. Check your local law.

## Credit

The concept — passive BLE plus Wi-Fi surveillance detection with a decaying
threat score on a pocket-sized ESP32 — comes from
[simeononsecurity/eye-spy](https://github.com/simeononsecurity/eye-spy)
(Apache-2.0). Argus is an independent implementation for different hardware:
ESP-IDF rather than Arduino, a web console rather than an RGB LED, and vendor
tables generated from the IEEE registry rather than maintained by hand. No code
was taken from it.

## Licence

MIT. See [LICENSE](LICENSE).
