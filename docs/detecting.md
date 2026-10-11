# How it decides something is worth reporting

The classes, the scoring, and the walks that reshaped them. The summary is in
[What it does](../README.md#what-it-does); this is the reasoning underneath it.

## The one claim persistence cannot make

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

## What a drone broadcasts about itself

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

## Choosing which monitors run

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

## The radio is shared, and it shows

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
