# Firmware, updates and flash layout

What the device knows about its own build, how an update is installed and
rolled back, and where it all sits in flash.

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

### Reading a manifest that outgrew its buffer

The device wants exactly one string from the update manifest — the version —
and used to hold the whole document to get it, in a 2,048-byte static. The
published manifest is **2,772 bytes** and lists every board, so it grows
whenever a board is added. The read kept a prefix, and it worked only because
the field it wants happens to be the first one in the file.

There *was* a size guard, and it never fired. Cloudflare serves this over
HTTP/1.1 with `Transfer-Encoding: chunked` and no `Content-Length`, so
`esp_http_client_fetch_headers()` reports no length and the guard was skipped
on every check. It was also a landmine: had the server ever sent a
`Content-Length`, the guard would have refused a 2,772-byte document outright
and **every device would have stopped seeing updates** — on a server
configuration change nobody would connect to the firmware.

The body is now scanned as it arrives, through a 768-byte window that carries
its tail forward between reads. The document can be any size, the field can be
anywhere in it, and the 2 KB static is gone — so the boards with the least RAM
get some back.

**The window's recycle point is the thing that needed testing**, and the first
test of it was worthless. It split documents between *feeds* and passed with
the tail-carry deliberately removed, because the documents were shorter than
the window, so the window never filled and the recycling code never ran. The
test now places the field at every offset across the recycle point; removing
the carry fails eighteen of them.

A manifest longer than 64 KB is refused. Not for memory — there is no buffer
to overflow — but because a far end serving a captive portal or an error page
is not worth reading through a radio to the end.

## Cutting a release

```bash
git tag v0.14.0 && git push origin v0.14.0
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

### Upgrading to v0.14.0

Nothing stored is lost and nothing needs doing by hand, but three things
change what a device already running v0.13.0 will do, and two of them make it
report *less*.

**The census now acts.** v0.13.0 learned what was always around and suppressed
nothing. This one quiets a household device that is identifiable, and halves
the weight of one whose advert shape may describe a population rather than a
device. A protected class is never either. So a board that has been running
for a fortnight will report fewer things than it did — and it says so: the
verdict band carries the count of what it quieted, because "clear" and "clear,
two quieted as household" are different claims.

**Four board profiles track half as many devices at once.** Both 2.8" Cheap
Yellow Displays, the 3.5", and the NM-CYD-C5 hold 96 rather than 192. The
nearby list is correspondingly shorter in a crowded place. That is what bought
the 3.5" board the memory to complete an over-the-air update at all, which it
could not do before — twelve failed checks in six hours, each driving its heap
to 56 bytes.

**The clock has to say where it came from.** A device reports no time until
something actually sets it, where v0.13.0 trusted any plausible-looking
number. In practice this changes nothing except on a board with no RTC chip
and no reachable NTP server, which now says `clock not set` rather than
guessing — and can be given the time from a browser at the console.

The run record's format went to 2 for the cell voltages, and version 1 records
are carried forward rather than discarded. The mesh is off unless built in.

## Partition layout

```
nvs        data nvs       0x9000     24K
phy_init   data phy       0xf000      4K
otadata    data ota       0x10000     8K
ota_0      app  ota_0     0x20000  1920K
ota_1      app  ota_1     0x200000 1920K
coredump   data coredump  0x3E0000   64K
```

**The table is OTA-shaped and OTA uses it**, and that was deliberate from the
start. The layout is the expensive part to change later: the browser
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

Slot occupancy today is **C5 83%, C6 76%, S3 66%, ESP32 69%**. The C5 is the
tightest and the fastest growing, so CI builds every target and would fail there rather than
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

### The coredump partition, and what it cost

The 3.5" CYD has ended five runs in a panic, 3.5 to 9.5 hours apart, with a
healthy heap. The panic handler prints a backtrace to a serial port that
nobody is watching at three in the morning. Two attempts to be watching
failed — one of them by capturing the thing and then losing the log — and the
run record could say *that* a panic happened but never *why*.

So the table gained a 64 KB `coredump` partition. The 4 MB layout was exactly
full, so the space came from the application slots: 1984K each down to 1920K.
The largest image this project builds is 1594 KB, which leaves a third of a
slot spare, and CI builds every target so an overflow fails there rather than
in the field.

**Both slots shrink by the same amount**, which is not tidiness. An OTA writes
into whichever slot is *not* running, so a mismatched pair is a trap: an image
that fits the slot you tested would fail on the other, half the time.

**64 KB is left spare above the coredump** rather than given back to the
slots. [#185](https://github.com/Forged-in-Feathers-Technology/observore/issues/185)
needs an `nvs_keys` partition if NVS encryption is ever enabled, and a
partition table change cannot reach an existing device over the air — the
browser flasher writes the table and OTA does not. The cable upgrade should be
spent once.

**Devices still on the old table are safe.** `esp_core_dump` finds its
partition by name at runtime, so a board without one simply never writes a
dump. It does not fall back to a fixed offset, which on an old table would be
the tail of that device's own `ota_1` slot.

### The device reports its own crash

A dump nobody reads is the same as no dump, so the next boot says what killed
the last one:

```
I esp_core_dump_flash: Found core dump 29376 bytes in flash @ 0x3e0000
E observore.crash: the previous run panicked in task "main" at pc 0x4009092d
E observore.crash: backtrace: 0x4009092d 0x400908f5 0x40096812 0x400dd79f 0x401aa49b
W observore.crash: the dump is kept in flash; read it with espcoredump.py or clear it from the console
```

Those addresses go straight into `addr2line` against the matching `.elf`:

```
0x400dd79f: app_main at main.c:676
```

ELF format rather than binary, because that is what gives
`esp_core_dump_get_summary()` a decoded backtrace on Xtensa — the architecture
the panicking board uses. RISC-V stores a raw stack dump instead, so the C5
and C6 report the trap cause (`mcause`, `mtval`, `ra`, `sp`) and the stack
needs `espcoredump.py` offline.

The dump is **kept, not erased on read**: the summary is a lead, the dump is
the evidence, and a report that vanished when somebody glanced at the console
would be the worst of both. `POST /api/crash?clear=1` is the only thing that
removes it, and `/api/status` carries the one-line summary as `crash`.

**Verified before being trusted**, which matters for something whose whole job
is to catch a fault that happens twice a day. A throwaway build with a
deliberate `abort()` was flashed to a 2.8" board: it panicked, saved 29,376
bytes, rebooted, printed the summary above, and the backtrace decoded to the
exact line the abort was on. Waiting days for a real panic only to find
nothing had been written would have been the expensive way to learn that.

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
