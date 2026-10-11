# Internal RAM, and the four times it ran out

This firmware is limited by internal RAM, not by flash or CPU. Every fault here
was found on hardware, and each one is written down so the next one is
recognised faster.

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

**That figure was measured against the wrong pool** — see [The heap figure was
measuring the wrong pool](#the-heap-figure-was-measuring-the-wrong-pool). It
was `MALLOC_CAP_INTERNAL`, which on the classic ESP32 includes 32-bit-only IRAM
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
