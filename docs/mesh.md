# Nodes that warn each other

Several boards in one household can tell each other what they have seen. Off by
default, four tiers deep: listen, warn, sign and count.

## Warning the neighbours

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
transmits`, and `/api/status` reports `peers: {nodes, warnings, tx, points,
trusted_points, stranger_points}`, where `nodes` counts distinct node ids and
the three point figures are what other nodes added to the score. The
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

## Warnings that say where

A warning that says *what* is worth less than one that says *what, and
where*. The wire format has carried a position since the first version and
the receiver has decoded it since — and nothing ever filled it in, so every
warning this project sent for its first several releases was positionless.
The field was shipped, tested, and dead at the sending end.

It is filled from a position the **owner gives the node**, not from a sensor:

```
POST /api/nodepos?lat_e7=515074000&lon_e7=-1278000
GET  /api/nodepos      -> {"ok":true,"set":true,"lat_e7":515074000,...}
POST /api/nodepos?clear=1
```

Degrees times ten million, because that is what the wire carries and because
a decimal point in a query string is a rounding argument nobody needs. 51.5074
is `515074000`.

### Why the owner first, and a GPS second

A stationary node knows where it is the moment somebody tells it, and most
nodes are stationary — that is the premise of learning what is normally
around. A GPS costs a part and a UART and is fitted to one board profile, so
it is the second source, not the prerequisite.

Where there is a receiver, a fix fills the position in when nobody has set
one. The owner's answer wins when there is one: a typed position is a
deliberate statement about where a node lives, and silently overriding it
with a receiver that has just acquired a marginal fix would make the setting
look broken. A node that moves is a node nobody pinned, and that is the case
the GPS branch is for.

The NMEA parser had been reading straight past the coordinates for several
releases. It was written to set a clock, and the position was sitting in the
same GGA sentence the whole time.

Two things about reading it that are easy to get wrong:

- **NMEA glues degrees to decimal *minutes*** — `ddmm.mmmm`, not decimal
  degrees. `5557.1980` is 55 degrees and 57.198 minutes, which is 55.9533
  degrees and not 55.5720. Reading it as a decimal degree is wrong by up to
  forty per cent of a degree, which is tens of kilometres, and looks entirely
  plausible.
- **A module without a fix may still fill the fields** with the last thing it
  believed. Quality zero means no position here, whatever the fields say,
  because a stale fix broadcast as current is worse than sending none. Losing
  a fix clears the position with it.

### There is no default, and that is deliberate

Unset means warnings go out exactly as they did before. Nothing is inferred
from a timezone, from an IP, or from anything else: broadcasting where a
detector is sitting is its own hazard, and a device that guessed would be
making that choice on the owner's behalf.

It is also only ever sent by a node that is already transmitting. Mesh
broadcast is off by default, and off means the advertising code is not
compiled in — so setting a position on a silent node changes nothing about
what leaves it.

### Coarse on the wire, with no setting to make it finer

Stored exactly, transmitted divided by 256 — about 2.8 metres. There is no
option to send more precision, because a node broadcasting its exact location
would be offering a resolution that only helps somebody looking for it.

The validity rule is one function, `observore_peer_position_usable()`, shared
by the encoder and by whatever stores a position, so a coordinate refused at
transmit time is refused at the point a person typed it instead. Null Island
is excluded: 0,0 is in the Gulf of Guinea and is overwhelmingly more likely to
be an unset field than a node on a buoy.

### Reading them back

`GET /api/peers` lists the standing warnings — node, class, age, whether the
sender was trusted, and the position when one was sent. The function behind
it, `observore_peer_recent()`, existed and was host-tested and had **no caller
in the firmware**: a node could report that three neighbours were warning and
nothing could say what about. Counting is not reporting, and a position that
cannot be read back is not worth sending.

## Warnings your own nodes signed

`trusted` was permanently false until now: the field existed so the parser
would know a tag might follow and not mistake it for payload, and nothing
could ever set it. One household secret fills it in.

**One shared key, not a keypair each.** Per-node keys would need a pairing
step for every pair of boards, and a key id inside a four-byte field that is
already the thing holding a signed, positioned warning inside a 31-byte
legacy advert. A household secret needs no pairing, and revocation is "change
the key on the nodes you still trust", which for a handful of boards is a real
answer rather than a shrug. The cost is stated rather than hidden: one
compromised node compromises the tier, because every node holds the same
secret.

`POST /api/mesh?key=<64 hex characters>`, and an empty key clears it. Stored in
NVS and **never read back**, like the notifier tokens — `/api/status` reports
`peers: {..., trusted, key}`, where `key` is only whether one is set. Nothing
serves it, because a key served over the LAN is a key on the LAN, and until
[#11](https://github.com/Forged-in-Feathers-Technology/observore/issues/11)
that LAN is plain HTTP. For the same reason the device does not *generate* one:
showing a secret would mean a second ritual like the console password's serial
print, and taking a key the owner already has avoids inventing it.

**Why thirty-two bits of tag is enough.** Truncated HMAC-SHA256. A blind
forgery succeeds about once in four billion, and the receiver already refuses
a sequence that does not advance, so an attacker cannot grind against a live
node. More to the point, the asymmetry bounds the prize: a stranger may warn
and nothing may silence, so forging a tag buys *more weight* on a warning,
never blindness — and not much weight, since a forged tag is one trusted voice,
which lifts a board to caution at most (see
[what a warning adds to the score](#what-a-warning-adds-to-the-score)). What it does not survive is a weak key — a four-byte tag
confirms an offline guess — so the key wants to be long and random rather than
a passphrase somebody chose.

**Verification uses the bytes that arrived**, not a re-encoding of the parsed
fields. Re-encoding would mean any difference between sender and receiver — a
clamped age, a quantised coordinate — failing verification and reading as a
forgery. `observore_peer_tag_span()` hands back exactly what was signed, and
refuses a payload that claims a tag which did not arrive, because verifying
against bytes past the end of a buffer is the obvious attack on a truncated
advert.

**If signing fails, the warning goes out untagged.** A tag of zeroes is still a
tag to a receiver, and one that fails to verify is indistinguishable from an
attack. Better to be a stranger than to look like an attacker.

Verified across two boards, in three states seven minutes apart:

| the two nodes | nodes | warnings | trusted |
|---|---|---|---|
| same key | 1 | 1 | **1** |
| different keys | 1 | 1 | **0** |
| same key again | 1 | 1 | **1** |

The middle row is the one that matters. `trusted` falls to zero while `nodes`
and `warnings` stay at one: the warning is still heard, still counted, still
reported — just not trusted. And it recovers, so the tier does not latch.

**A trusted warning raises the score, by a bounded amount.** The tier was made
real and visible before it was allowed to act, which is the order the census
took; the amount is set out in
[What a warning adds to the score](#what-a-warning-adds-to-the-score).

No position is sent. The format carries one and the decoder reads one, but
this device has no fix to put there; a GPS node is where that field starts
being filled, and the encoder refuses zeroes rather than claiming the Gulf of
Guinea.

## What a warning adds to the score

**A warning is a second, separate term in the score, added after the local
sum.** It names a class, not a device, and nothing on this node saw it, so it
never becomes a row in the device table. It is only ever added, which is the
whole reason no warning can lower a score, quiet a device or change what a
protected class reads.

The numbers, all of them in `main/observore_peer.h` where the build refuses
to compile if they are retuned out of the corner they have to sit in:

| constant | value | what it bounds |
|---|---|---|
| `OBSERVORE_PEER_TRUSTED_POINTS` | 3 | what one trusted node adds, per class |
| `OBSERVORE_PEER_STRANGER_POINTS` | 1 | what all strangers about one class add, together |
| `OBSERVORE_PEER_STRANGER_CAP` | 2 | every stranger point, across every class |
| `OBSERVORE_PEER_CLASS_CAP` | 6 | the most any one class can get from peers |
| `OBSERVORE_PEER_TOTAL_CAP` | 6 | everything peers add, one alert at most |
| `OBSERVORE_PEER_STALE_AGE_S` | 255 | the sender's age at which a warning counts nothing |

**Trusted nodes add 3 each, per class, and a class stops at its own weight.**
Three is the one value that lets a single friend reach caution, keeps a single
friend below alert, and lets two agreeing friends reach alert. A class's peer
points also stop at the smaller of the class cap and the class's own weight, so
a friend's warning stands in for one sighting rather than for a crowd of them,
and all peer points together stop at 6. One node repeating itself about the
same class adds nothing more: it renews the hold and adds nothing.

**One friend cannot raise an alert on their own.** One trusted node about a
class this node does not see lifts the board to caution at most, and one
trusted node confirming a class worth less than an alert stays below alert too.
Two or more distinct trusted nodes may alert, but only about a class that is
worth an alert by itself. Because each class's peer points stop at its own
weight, two friends warning about a tracker, smart-glasses, drone or hunter
that this node does not see reach 3, caution, never alert: **only alpr,
bodycam and deauth-flood can reach alert from peers alone.** Local evidence
that alerts on its own still alerts, and a friend's warning does not take that
away.

**Strangers are one voice per class, and a quiet one.** All strangers about a
class together are worth 1 however many node ids they invent, all classes
together are worth at most 2, and they only attach to a class this node already
counts. They can never change the level: in practice a stranger adds at most
1, because a second counted class already puts the board at alert and there is
no room left under it. The rule does not lean on that. Strangers are only ever
given the room left in the level the score is already in, so even a counted
class that scored below its weight cannot be lifted over a level boundary by
them.

**"Counts" means a device of that class is in the current sum.** A device the
census has quieted does not confirm a stranger's warning, because it is not
counted; and every class a warning can score is protected, so the census never
quiets one of them in the first place.

**Only some classes score.** Tracker, smart-glasses, drone, hunter, alpr,
bodycam and deauth-flood are worth caution or more, and those are the ones a
warning can add to. Camera, fleet-telematics, accessory, follower, tailing and
anything unidentified are heard and shown, with 0, and add nothing: a warning
about a fixture is a fact about the room, and the cheapest attack on a mesh is
to fill it with true but useless statements.

**A warning about a class whose monitor is switched off here adds nothing from
any peer, trusted or not.** Switching a monitor off on this node also switches
off what any peer can add for that class, which is the promise
[docs/detecting.md](detecting.md) already makes: nothing automatic, a mesh peer
included, overrides the owner's off switch. The warning is still heard and still listed, as not counted and
labelled `monitor off here`, so the silence is visible rather than assumed.

**A warning holds, then it stops.** It counts while its age plus the time since
this node heard it is under 300 seconds. An age of 255, the sender's saturated
"a while ago", counts nothing, because a sender keeps announcing a device for
up to half an hour after it last saw it and every receiver would otherwise hold
the points for a threat that has left. There is no fading: the full value
until the hold ends and then none, because the score has no accumulator to
bleed.

Six scenes, each checked by the host tests:

| what this node sees | what peers say | score | level |
|---|---|---|---|
| nothing | one friend: a bodycam | 3 | caution |
| nothing | two friends: a bodycam | 6 | alert |
| nothing | two friends: a Flipper (hunter) | 3 | caution |
| a Flipper | one friend: a Flipper | 5 | caution |
| a Flipper | two friends: a Flipper | 6 | alert |
| a Flipper | any number of strangers: a Flipper | 4 | caution |

**Where it shows.** `/api/status` `peers` carries the three point figures,
`/api/peers` lists every held warning with its counts, points, whether it was
seen here and why it did or did not count, `/api/history` `peer_events` holds
what peers added during this boot (RAM only, and gone on reboot), the console
banner and findings table and its Mesh warnings panel, the coloured band and
rows on a screen board, the log line when a level changes, the notification
note, and one card row per trusted report. Every place that lists peer points
names the node and whether it was trusted. The stranger voice is named by its
lowest node id, the one the rule picks, with how many stranger nodes it speaks
for, so the same warnings read the same whichever order they arrived in.

**Known limits, said plainly.**

- A captured trusted warning that is replayed after its hold counts again. It
  is still one node, so it lifts a board to caution at most; the table
  refuses a repeated sequence only while the entry is live, and accepts it
  again once the entry has expired.
- A forged tag is one trusted voice. Thirty-two bits is not a strong
  signature, and what keeps a forgery cheap to survive is the bound above, not
  the tag.
- Two friends whose sixteen-bit node ids collide count as one node. That errs
  low: the board hears less than it was told, never more.

## Hearing other nodes, without becoming findable

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
in a way that silent suppression is not. It holds by construction: there is no
suppression path at all, and a stranger's warning adds at most one point on a
class this node already counts — folded to one voice however many ids appear,
never enough to change the level, never a way to quiet anything
([the rule](#what-a-warning-adds-to-the-score)). The device says what it heard
and whose it was, and a person decides.

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
- **One entry per node and trust.** A node repeating itself must not be able to
  fill the table and push other nodes out. Strangers are evicted first and can
  never evict a live trusted entry, and a stranger who uses a friend's id gets
  an entry and a sequence window of its own, so it cannot overwrite the
  friend's or make the friend's next warning look like a replay.
- **Warnings expire**, after five minutes. A warning is about *now*; one kept
  for ever would let a single sighting look like a standing alarm.
- **A version we do not know is refused.** Unlike another project's format this
  one is ours, and reading fields by a layout we have since changed is how a
  position ends up in the wrong place.

A tag field is parsed for its length and not judged there. A tag that cannot be
checked must never be mistaken for one that has been, so the parser leaves
`trusted` false and the receive path sets it, after checking the tag against
the household secret (see [Warnings your own nodes signed](#warnings-your-own-nodes-signed)).

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

`/api/status` reports `peers: {nodes, warnings}` and the three point figures,
`points`, `trusted_points` and `stranger_points`. `nodes` counts distinct node
ids, and `warnings` the standing entries, so a node heard both signed and
unsigned is one node with two warnings. This is how a headless receive-only node
says whether it is hearing anything at all, and what it did about it.
