# Observore

[![CI](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml/badge.svg)](https://github.com/Forged-in-Feathers-Technology/observore/actions/workflows/ci.yml)

A passive counter-surveillance detector for the [Seeed Studio XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/),
built by [Forged in Feathers Technology](https://www.forgedinfeatherstechnology.com).

It tells you what is watching you. Built to sit in one place and watch that
place: it learns what is normally there, then reports what is new. It listens
for the radio signatures of body cameras, licence-plate readers, IP cameras,
Bluetooth trackers, smart glasses and Remote ID drones, scores what it finds,
pushes notifications, and serves the log on your network.

Observer and carnivore: it eats surveillance signals.

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
continuously. If it does not answer, it is patrolling; wait, or hold the button
to bring it up now.

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

**1. At runtime, from the console.** Hold the button, join the SoftAP, and fill
in the **Network** panel. Stored in NVS. Nothing touches this repo at all, and
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
| 201 | network not found — check the SSID, and that it is 2.4 GHz |
| 202, 15, 204 | authentication or handshake failed — wrong password |
| 203 | association refused — MAC filtering? |

The ESP32-S3 has no 5 GHz radio, so a 5 GHz-only SSID can never be joined.

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
  the next window, or hold the button.

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

Configure the LED pin, button pin and hostname under `idf.py menuconfig` →
**Observore**.

### The console password

Each device generates its own random console password on first boot, stores it
in NVS and prints it on the serial log:

```
console SoftAP: "console-F964FD"  password: 2p78ggedb4bj
```

It is not derived from anything observable and it is not shipped in this
repository, so no two devices share one. Deriving it from the MAC would be
pointless — the SoftAP's BSSID is in every Wi-Fi scan and the derivation would
be right here in the source. A build-time constant would be worse still:
everyone who flashed the firmware would share it.

It is deliberately **not** exposed over the network. Serving the console's own
password from inside the console would turn "someone was on the LAN once" into
"someone can join the SoftAP in range, indefinitely". Read it from serial;
erasing NVS generates a new one.

`OBSERVORE_AP_PASSWORD` overrides it, and every device built from that firmware
then shares the password you chose. `tools/check_no_secrets.sh` fails if such a
value reaches a tracked file.

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

CI runs these on every push, alongside the credential scan, an ESP-IDF build
for the esp32s3, and a check that the generated OUI table still matches what
its generator produces.

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

Observore pushes to a [Gotify](https://gotify.net/) server. Configure it in the
console's **Notifications** panel, or:

```
GET  /api/notify
POST /api/notify?url=https://gotify.example.com&token=YOUR_APP_TOKEN
POST /api/notify?test=1
POST /api/notify?clear=1
```

It POSTs `{"title","message","priority"}` to `<url>/message` with the token in
an `X-Gotify-Key` header. Priority is mapped from what was found: 8 for a
bodycam or ALPR, 7 for a follower or tracker, 5 for a drone or smart glasses,
2 for a camera. Escalations of the overall threat level are pushed too; drops
are not, because an alert that clears is not news.

The token is stored in NVS and is **write-only** from outside the device — no
endpoint returns it, exactly like the Wi-Fi password.

**Sending needs the uplink.** Detections happen during patrol, which has no
network, so notices are queued and flushed the next time you are joined to your
network. The queue holds 24 and drops the oldest when full: a detector that
stops noticing new things because its outbox is full would be worse than one
that loses the oldest notice. A failed send stays queued and is retried rather
than discarded.

The console shows sent, queued, failed and dropped counts, plus the last
transport error — a `401 (bad token)` is reported as such rather than as a bare
number.

## A note on internal RAM

The ESP32-S3 has about 180 KB of DRAM regardless of how much PSRAM is fitted,
and Wi-Fi and lwip allocate from it. Observore therefore builds its JSON responses
in **PSRAM**, not in static internal buffers.

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
