# Ignoring what you already know about

Everything the device does to stop reporting the room it lives in: hand-set
mutes, the baseline, and the census it builds by itself.

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
