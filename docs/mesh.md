# Nodes that warn each other

Several boards in one household can tell each other what they have seen. Off by
default, three tiers deep: listen, warn, and sign.

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
never blindness. What it does not survive is a weak key — a four-byte tag
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

**A trusted warning does not yet change the score.** Warnings still contribute
zero, exactly as before. Making the tier real and visible before letting it
act is the same order the census took, and "how much more should a friend's
warning count" is a separate decision with its own way of going wrong.

No position is sent. The format carries one and the decoder reads one, but
this device has no fix to put there; a GPS node is where that field starts
being filled, and the encoder refuses zeroes rather than claiming the Gulf of
Guinea.

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
