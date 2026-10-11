# Knowing what time it is

A board with no battery-backed clock still has to timestamp what it sees. Where
the time comes from, and how it says which source set it.

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
false, the absolute fields are empty, and the console and the system page both
say `clock not set`. Only the relative "4m ago" is shown. Emitting 1970 dressed
up as a timestamp would be worse than admitting the clock is unset, on a device
whose output is meant to be evidence.

### The clock says what set it

`observore_clock_valid()` used to be one comparison: is the system clock past
2025? That conflated two different claims — "the clock holds a plausible
number" and "the device knows what time it is" — and anything that seeded the
clock would have satisfied the first while the second stayed false.

That became load-bearing when the census started suppressing things. The
census counts *separate days*, so a clock that never advances records every
sighting on one day and nothing ever becomes furniture; a clock set to the
wrong day records furniture against a date it will not be judged against.
Neither failure announces itself.

So the clock carries a **source**, and `valid` needs both halves — something
actually set it, *and* the result is a plausible date:

| source | what it is |
|---|---|
| `none` | never set: the time is whatever boot left behind |
| `person` | handed in from a browser at the console |
| `chip` | read from the board's own RTC at startup |
| `network` | SNTP, which is the one that is actually right |

The order is a ranking, and a better source overwrites a worse one without
being asked — which is how a person's rough answer gets quietly corrected the
moment the network arrives. A worse source cannot undo a better one, because a
browser tab left open on the console offers to set the clock on every reload
and must not be able to replace an SNTP answer with its own opinion.

**The source lives in RTC memory, with the clock it describes.** System time
survives `esp_restart` — which here means an update installing itself, or a
panic or watchdog — because ESP-IDF keeps the boot time in RTC slow memory. On
the bench it survived an EN-pin reset as well: a watch reset over USB came back
holding both the time and `network` as its source, and correctly declined its
own RTC chip's answer in favour of the better one it already had. It does not
survive a power cycle. A source kept in an ordinary static would be lost on a restart
while the clock it describes survived, turning a known time into an unknown
one across every OTA. A source kept in NVS would do the opposite and outlive
the clock, claiming a synced time on a board that has just been plugged in.
That memory has exactly the right lifetime, which is the whole reason for
using it rather than either. It is read through a magic word, because it is
not initialised at power-on and would otherwise report whichever source the
previous occupant's bits happened to spell.

**One door to the census.** `observore_clock_day()` is the only thing that
turns the clock into a census day, and it returns "no day" whenever the source
is `none`. `observore_census_day()` stays pure arithmetic that converts
whatever timestamp it is handed — the host tests drive it directly — and
deciding whether that timestamp is worth believing is a question for the clock.
Asking it at four separate call sites is how three of them end up still asking
the old way.

### Setting the clock from the browser

Most of these boards have no RTC chip, and the places this device is worth
carrying are routinely places where no NTP server is reachable: a camera VLAN
with no route out, a field with no uplink at all. Those boards cold boot into
1970 and stay there — findings with no date, and a census that cannot count a
single day.

The browser on the other end of the console already knows the time to the
millisecond. `POST /api/time?epoch=<seconds>` hands it over, and the console
shows a **set the clock from this browser** button whenever the time did not
come from the network. It is a worse clock than SNTP and a far better one than
none, and it is recorded as what it is rather than passed off as a sync.

It is a *source*, not an override:

- **A time outside the window a running device can be in is refused, not
  clamped.** The floor is 2025-01-01, which keeps an unsynced ESP's 1970 out.
  There is a ceiling at 2100 too, because a time set by hand can be wrong in
  the other direction — a browser with its year typed wrong is no more usable
  than 1970, and a date past what the census day number can hold would be
  stored as a different date entirely. A device that will not use a time is
  honest; one that silently moves it to the nearest allowed instant has
  invented a date and will timestamp evidence with it.
- **It goes straight into the RTC chip where there is one**, so a time handed
  in by a person survives the power cycle that loses it everywhere else.
- **It is behind the console password**, like everything else there. Not
  because the time is secret — because a clock now decides what the census
  suppresses, and an unauthenticated endpoint that moves the date is an
  unauthenticated endpoint that decides what the device stops reporting.
- **The two refusals say different things.** A time outside the window is
  yours to correct; a clock already set from somewhere better is not a problem
  at all. The first version answered both with one message and could not say
  which had happened, which the bench demonstrated within a minute.

The decision itself is a pure function, `observore_clock_rule()`, and the host
tests drive it. The rest of the clock needs a real `settimeofday()` and a chip
on an I2C bus, which is exactly how a rule like this ends up never exercised.
One of the cases it pins down is the half that is easy to leave out: a recorded
source whose clock reads 1970 must not outrank a real answer, or a device would
refuse every source forever and could never be dated again.

**Notifications carry the time the thing was seen**, not the time the message
was sent. Notices are queued while patrolling and flushed on the next uplink,
so delivery can trail detection by twenty minutes, and the queue exists
precisely for the case where that gap is longest.

Both forms are reported: `last_seen_s` counts seconds ago, `last_seen` is
ISO-8601 UTC. A client with no clock of its own still needs the first.
`/api/status` also reports `clock`, which is the source name, and the system
page on the screen shows the time with the source beside it — `not set` where
there is none, and where to set it.
