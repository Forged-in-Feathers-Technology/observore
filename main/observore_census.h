#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "observore_types.h"

/* The exchange option is a build-time switch, so the header has to see it on
 * the device. The host suite has no sdkconfig and names the option on the
 * command line instead. */
#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#endif

/* What is always around.
 *
 * A stationary node learns the furniture: the doorbell, the printer, the
 * neighbour's television, the household's own phones. The point of knowing
 * that is to stop reporting it -- see #132.
 *
 * Learning and acting were deliberately separate changes rather than one, and
 * not for convenience. Twice this project has silenced the thing it exists to
 * notice: a baseline that blinded the device outright (v0.9.1), and a
 * baseline that quieted a Flipper Zero by name for a week (#123). Both were
 * acting on a judgement that had never been watched. So membership was
 * earned, persisted and reported for four days first, and what came back
 * changed the plan: see `observore_census_verdict` for the measurement that
 * ruled out quieting half of what this was meant to quiet.
 *
 * ## Days, not hours
 *
 * Membership is earned across separate days and never within one evening.
 * "Furniture" and "here right now" are different claims, and conflating them
 * is the mistake behind every revision of the follower class -- a device at
 * your elbow for three hours is interesting precisely because it is not
 * furniture.
 *
 * So each identity carries a bitmap of the days it was seen on. One bit per
 * day, the newest at bit 0, shifted along as days pass. Decay needs no code:
 * a day that falls off the end of the window is forgotten because the bit
 * went with it. A visitor's phone seen three days running stops being
 * household a fortnight after they leave.
 *
 * ## Identity is the caller's problem
 *
 * The id here is opaque. Whether a household member is an address, a
 * fingerprint, or something else is a question with real consequences -- an
 * address rotates, and a fingerprint identifies a *kind* of device, so two
 * identical handsets share one -- and it is not settled by this file. What is
 * settled here is the rule about days, which is the part that was worth
 * getting right before anything depends on it.
 */

#define OBSERVORE_CENSUS_MAX       64  /* identities tracked */
#define OBSERVORE_CENSUS_WINDOW    16  /* trailing days: the width of the mask */
#define OBSERVORE_CENSUS_MIN_DAYS   3  /* distinct days before it is furniture */

/* A day with no number.
 *
 * The device detects from the moment it powers on and only learns the time
 * when it next reaches a network, so early sightings genuinely have no date.
 * Counting them against day zero would hand membership to whatever happened
 * to be in the room during the first minute after every boot, which is the
 * opposite of earning it over days. They are dropped instead. */
#define OBSERVORE_CENSUS_NO_DAY   (-1)

/* The last day the record can hold, because `last_day` is sixteen bits: day
 * 65535 is in 2179. A date beyond it is refused rather than stored, since a
 * truncated day number is not a near miss -- it silently claims a different
 * date, and the window would then be measured from it. A test asking about
 * day 100000 is what turned that up, and the bound lives here rather than in
 * the test. */
#define OBSERVORE_CENSUS_DAY_MAX  65535

/* How many distinct addresses one identity is tracked under.
 *
 * Eight, to match OBSERVORE_MUTE_ADDRESS_LIMIT -- the count at which the mute
 * store retires a fingerprint rule for describing a kind of device rather than
 * one. Measuring the census against the same number means the two can be
 * compared directly when the suppression slice has to choose a threshold.
 *
 * What this measures, and what it does not. An identity seen under many
 * addresses is ambiguous: it is either one device rotating its address, which
 * is exactly what keying on the advert fingerprint is *for*, or several
 * identical devices sharing a shape. The census cannot tell those apart, and
 * neither can anything else here. What it can say is how often the ambiguous
 * case arises at all -- which is the fact that decides whether suppression
 * keyed on a fingerprint is viable, and the reason this is being measured
 * before anything is suppressed.
 */
#define OBSERVORE_CENSUS_ADDRS 8

typedef struct {
    uint32_t id;        /* opaque: whatever the caller uses to mean "this device" */
    uint16_t days;      /* bit 0 is `last_day`, bit n is n days before it */
    uint16_t last_day;  /* the day bit 0 refers to */
    /* Sixteen-bit hashes of the addresses this identity has been seen under.
     * Hashes rather than addresses: the question is how many, not which, and
     * six bytes apiece would quadruple the table for an answer nobody needs.
     * They do not decay -- a device that rotated through eight addresses last
     * month really has been seen under eight, and that is the measurement. */
    uint16_t addr[OBSERVORE_CENSUS_ADDRS];
    uint8_t  addr_n;    /* how many of addr[] are in use */
    uint8_t  addr_over; /* a further distinct address arrived with the set full */
} observore_census_entry_t;

/* The stored format. Version 1 was this table without the address set, and is
 * discarded rather than read: at eight bytes an entry against this one's
 * larger size, an old blob with an even number of entries divides evenly into
 * the new size and would restore as half as many entries of garbage. That is
 * the same trap the touch calibration hit, and the answer is the same one --
 * record which code wrote it rather than trying to recognise the shape. */
#define OBSERVORE_CENSUS_FORMAT 2

/* The local day a timestamp falls in, or OBSERVORE_CENSUS_NO_DAY.
 *
 * Local rather than UTC, because the rule is about evenings. An evening that
 * straddles midnight UTC would otherwise count as two days, and two days is
 * two thirds of the way to being furniture.
 *
 * The count runs from 2000-01-01 and is derived from the broken-down date
 * rather than from the timestamp, so it moves with the configured zone and
 * with daylight saving. */
int observore_census_day(time_t when);

/* Same, from a date already broken down. Separated out so the day arithmetic
 * can be tested without a timezone in the way. */
int observore_census_day_from_tm(const struct tm *lt);

/* What the census calls one device.
 *
 * Moved here from the sweep's caller, because the scoring needs the same
 * answer and two copies of an identity rule are two rules. The fingerprint
 * where there is one, because the household's own phones rotate their
 * addresses and would never reach a second day otherwise; failing that a
 * fixed address; and nothing at all for a random address with no stable
 * advert shape, which has no identity to remember.
 *
 * Returns 0 when there is nothing to key on. */
uint32_t observore_census_id(const observore_event_t *e);

/* What the census is entitled to do about a sighting.
 *
 * This is where four days of measurement landed. The original plan was to
 * quiet household devices, and the data said that cannot be done safely: on
 * the bench board every single identity at the address ceiling had become
 * household, so the identities the census most wants to quiet are exactly the
 * ones whose advert shape may name a kind of device rather than one. There is
 * no safe subset.
 *
 * So two rules instead of one:
 *
 *   REPORT   nothing changes. Anything not household, and -- whatever else is
 *            true -- anything in a protected class. A census may quiet
 *            unidentified things and never the classes this device exists to
 *            find, which is the same rule a baseline already obeys and for the
 *            same reason.
 *
 *   QUIET    household, below the ceiling, not protected. Few enough
 *            addresses that the shape names one identifiable device: the
 *            doorbell, the printer, the things that do not rotate.
 *
 *   DAMPEN   household and at the ceiling. Fewer points rather than none. If
 *            the shape really is one rotating phone the noise goes away; if it
 *            covers a population, a stranger's device still registers. The
 *            failure mode is under-alarmed rather than blind, which is the
 *            trade worth making on a device that has been blinded twice.
 */
typedef enum {
    OBSERVORE_CENSUS_REPORT = 0,
    OBSERVORE_CENSUS_DAMPEN,
    OBSERVORE_CENSUS_QUIET,
} observore_census_verdict_t;

/* The verdict that acts: this node's own days and the days the owner's other
 * nodes reported, under one rule. It is observore_census_explain() without the
 * explanation, so the score and the reason for it cannot disagree. */
observore_census_verdict_t observore_census_verdict(uint32_t id, int day,
                                                    observore_class_t cls);

/* How many of the owner's other nodes one identity can name at once.
 *
 * Three covers a household of four nodes. A fourth friend competes for a place
 * by the days it brings, and loses a tie to one already counted: dropping a
 * contribution only ever makes an identity less household, which is the safe
 * direction. */
#define OBSERVORE_CENSUS_FROM 3

/* Why a verdict is what it is.
 *
 *   local     the verdict from this node's own days and addresses alone.
 *   merged    the verdict that acts: local days joined with the owner's other
 *             nodes' days. This is what observore_census_verdict() returns.
 *   via_mesh  true exactly when `local` is REPORT and `merged` is not -- a
 *             device that this node alone would still report. A merge that
 *             turns QUIET into DAMPEN raised the alarm, so it is not counted
 *             as quieting the owner's other nodes caused.
 *   from      the nodes whose days still fall inside the window, in ascending
 *             order, so a screen can name the friend rather than say "mesh".
 *             Empty when the exchange is not compiled in. */
typedef struct {
    observore_census_verdict_t local;
    observore_census_verdict_t merged;
    bool                       via_mesh;
    uint8_t                    from_n;
    uint16_t                   from[OBSERVORE_CENSUS_FROM];
} observore_census_why_t;

/* The merged verdict, and the reasoning behind it.
 *
 * `why` may be NULL. A protected class or an id of 0 gives REPORT and REPORT
 * with no friends named, before any table is read: the classes this device
 * exists to find are never the census's to quieten, from this node or any
 * other. */
observore_census_verdict_t observore_census_explain(uint32_t id, int day,
                                                    observore_class_t cls,
                                                    observore_census_why_t *why);

/* How much of its weight a dampened sighting keeps: half, never less than
 * one. Halving a single point would round to silence, which is the one
 * outcome this is designed to avoid. */
uint16_t observore_census_dampen(uint16_t points);

/* Lock order. The tracker's lock is taken first and the census lock inside it,
 * never the other way, because the tracker asks for the verdict with its own
 * lock held. Nothing under the census lock calls into the tracker or takes the
 * mute store's lock: the protected-class check reads a constant descriptor.
 * Readers copy out and never hold the census lock across network I/O.
 *
 * Call this before anything else touches the census: it creates the lock. */
void observore_census_init(void);

/* Record that `id` was seen on `day`, at address `mac` (six bytes, or NULL
 * when there is no address to attribute it to). Repeat sightings on the same
 * day at the same address are the normal case and cost nothing. */
void observore_census_note(uint32_t id, int day, const uint8_t *mac);

/* How many distinct addresses `id` has been seen under, saturating at
 * OBSERVORE_CENSUS_ADDRS. `over`, if given, is set when more arrived after
 * the set was full -- so "8" and "8 and counting" are distinguishable, which
 * matters when the number is being compared against a limit. */
int observore_census_addresses(uint32_t id, bool *over);

/* Whether `id` is furniture as of `day`: seen on at least
 * OBSERVORE_CENSUS_MIN_DAYS distinct days inside the trailing window. */
bool observore_census_is_household(uint32_t id, int day);

/* How many distinct days inside the window `id` has been seen on, 0 if it is
 * not known at all. For the console, and for seeing the rule work before
 * anything acts on it. */
int observore_census_days_seen(uint32_t id, int day);

/* How many identities are household as of `day`, and how many are tracked at
 * all. Either pointer may be NULL. */
void observore_census_counts(int day, int *household, int *tracked);

/* The address spread, in three buckets: identities seen under one address,
 * under two to seven, and at the eight-address ceiling.
 *
 * This is the shape of the measurement that decides whether suppression keyed
 * on an advert fingerprint is viable at all, so it belongs somewhere a person
 * can read without a browser and without a password. If most identities sit
 * in `one`, a fingerprint names a device. If many sit in `many`, a fingerprint
 * names a population, and quieting one would quiet the lot.
 *
 * Any pointer may be NULL. */
void observore_census_addr_spread(int *one, int *few, int *many);

/* Of the identities at the address ceiling, how many are household as of
 * `day`.
 *
 * This is the intersection that decides what the census may safely do, and it
 * is not derivable from the spread and the household count separately. An
 * identity at the ceiling is ambiguous -- one device rotating, or a population
 * sharing an advert shape -- and an identity that is household is one the
 * census would otherwise be entitled to quiet. Where those overlap, quieting
 * it would risk silencing a whole class of device, and the class this project
 * exists to notice could be in it.
 *
 * Reported rather than acted on: a number this consequential should be read
 * by a person before anything is built on it. */
int observore_census_household_at_ceiling(int day);

/* One identity as the console lists it: a copy, taken under the census lock,
 * so a reader sees a row that is consistent with itself while the table goes
 * on changing. Nothing here points into a table.
 *
 *   days, household, addrs, more   merged: this node's days and addresses
 *                                  joined with the owner's other nodes'.
 *   local_days, household_local    this node's own alone.
 *   via_mesh                       household, and not household_local: the
 *                                  owner's other nodes are why.
 *   here                           false for an identity only the owner's other
 *                                  nodes have seen.
 *   last_day                       this node's entry's, or the other nodes'
 *                                  when `here` is false.
 *   from                           the nodes whose days count, ascending. */
typedef struct {
    uint32_t id;
    int      days;
    int      local_days;
    bool     household;
    bool     household_local;
    bool     via_mesh;
    bool     here;
    int      addrs;
    bool     more;
    uint16_t last_day;
    uint8_t  from_n;
    uint16_t from[OBSERVORE_CENSUS_FROM];
} observore_census_row_t;

/* Fill row `i` as of `day`. Returns false past the end, so the listing is a
 * loop that stops there. This node's own entries come first in table order,
 * then, with the exchange, the identities only the owner's other nodes have
 * seen, in theirs. The table can change between calls; each row is whole. */
bool observore_census_row(size_t i, int day, observore_census_row_t *out);

/* Whether the table is full.
 *
 * Worth asking separately because a full census is a different thing from a
 * busy one: it is still counting days for what it holds, but a new identity
 * now costs an existing one its place. The bench board sat at the cap for
 * days showing a flat count and nothing said so, which is how a table that
 * had stopped learning looked exactly like one that had finished. */
bool observore_census_full(void);

/* Serialise the table, header and all, into `out`. Returns the number of
 * bytes the blob needs; with `out` NULL or `cap` too small it writes nothing
 * and returns that size, so a caller can ask first. */
size_t observore_census_blob(void *out, size_t cap);

/* Replace the table wholesale, from a blob previously saved. A blob whose
 * header does not say this code wrote it, or whose length disagrees with its
 * own header, is refused: restoring half an entry is worse than starting
 * empty. Returns false if the blob was rejected. */
bool observore_census_restore(const void *blob, size_t len);

/* The band's text while a census window is open, written into `buf` with
 * snprintf semantics clamped to len - 1; returns the length written.
 *
 *   " CENSUS SHARE 18s  CAUTION 12"    exchange mode with something to send
 *   " CENSUS LISTEN 18s  CAUTION 12"   listening only, or nothing to send
 *
 * The level word and the score stay in it. A node in a window is still a
 * detector, and a transmission must not be a way to hide its verdict: the band
 * is the one place a person at the device cannot miss, so the announcement and
 * the verdict share it. One leading space, because the longest form (LISTEN, 30
 * seconds, CAUTION, 99) is 30 characters and the narrowest panel is 31 columns:
 * the level word and the score are never the part that is cut. A NULL level
 * reads "?". Always compiled and pure, so the screen has no #if and the host
 * suite pins the wording. */
size_t observore_census_band_text(bool sharing, unsigned remaining_s,
                                  const char *level, unsigned score,
                                  char *buf, size_t len);

/* The census part of the bar: "3 quiet, 1 via mesh". The via-mesh figure is
 * part of the quiet total, not an addition to it (3 quiet, of which the owner's
 * other nodes caused 1), so the total means what it meant before the exchange.
 * "3 quiet" when friends caused none, and "" (length 0) when nothing is quiet.
 * Snprintf semantics clamped to len - 1. */
size_t observore_census_quiet_text(int quieted, int quieted_mesh, char *buf,
                                   size_t len);

#if CONFIG_OBSERVORE_MESH_CENSUS

/* What the owner's other nodes have seen.
 *
 * A friend's entries live in a table of their own, so a friend can never cost
 * this node an identity it learned itself: the local table above is not
 * touched by anything in this section.
 *
 * Each friend's days are kept separately rather than OR'd into one mask. That
 * is what lets a friend be named only while its days are still inside the
 * window, and what lets a merge be an OR that a repeat or a replay cannot
 * change: the days a friend reports are days that happened, and receiving them
 * twice or out of order leaves the table as it was.
 *
 * Addresses are one union, because the address ceiling asks how many, not who
 * saw them. */
#define OBSERVORE_PEER_CENSUS_MAX     CONFIG_OBSERVORE_PEER_CENSUS_MAX

/* Which code wrote the stored table, for the same reason as
 * OBSERVORE_CENSUS_FORMAT: record it rather than recognise the shape. */
#define OBSERVORE_PEER_CENSUS_FORMAT  1

/* How many bytes of the key check the stored table carries. */
#define OBSERVORE_PEER_CENSUS_CHECK_LEN 4

/* One identity as the owner's other nodes reported it. Every from[].days is
 * aligned to `last_day`, as bit 0 is. Node 0 marks a free place, because a
 * node id is never 0 (observore_peer_node_id() returns 1 in its place). */
typedef struct {
    uint32_t id;
    uint16_t last_day;
    uint16_t addr[OBSERVORE_CENSUS_ADDRS];
    uint8_t  addr_n;
    uint8_t  addr_over;
    struct {
        uint16_t node;
        uint16_t days;
    } from[OBSERVORE_CENSUS_FROM];
} observore_peer_census_entry_t;

_Static_assert(sizeof(observore_peer_census_entry_t) == 36,
               "a peer census entry is 36 bytes, which the stored blob relies on");
_Static_assert(OBSERVORE_PEER_CENSUS_MAX >= 16 && OBSERVORE_PEER_CENSUS_MAX <= 64,
               "the peer table is sized between 16 and 64 identities");

/* Where the exchange keeps its state.
 *
 * On an ESP32-S3 with PSRAM the peer table and the exchange's working set are
 * taken from the heap once, at init, preferring PSRAM. A static table sits in
 * internal DRAM, and on a screen board that is the memory a window needs: the
 * Waveshare 1.85" runs with under 2 KB of internal heap free before the
 * exchange is built in at all, and the 2.3 KB table at 64 identities came out
 * of that. Everywhere else the tables stay static, where their cost is fixed
 * and measured at build time against the C5 and C6 budget. */
#if !defined(OBSERVORE_HOST_TEST) && defined(CONFIG_IDF_TARGET_ESP32S3) && \
    defined(CONFIG_SPIRAM) && CONFIG_OBSERVORE_MESH_CENSUS
#define OBSERVORE_CX_STATE_IN_PSRAM 1
#else
#define OBSERVORE_CX_STATE_IN_PSRAM 0
#endif

typedef enum {
    OBSERVORE_CENSUS_MERGE_SAME = 0,   /* nothing changed */
    OBSERVORE_CENSUS_MERGE_CHANGED,    /* the stored entry's bytes changed */
    OBSERVORE_CENSUS_MERGE_REFUSED,    /* not a record this node will take */
} observore_census_merge_t;

/* Take one census entry a friend reported, as a record already parsed and
 * authenticated by the caller.
 *
 * `node` is the friend's id and `my_day` this node's census day. The entry's
 * days and addresses are added to what is held for that friend and nothing is
 * ever taken away, so the merge is an OR: a repeated, replayed or reordered
 * entry can only deliver days that already happened. It never touches this
 * node's own table.
 *
 * Refused, with nothing written: no entry, node 0, id 0, an empty day mask, an
 * address count above the ceiling, a `last_day` more than one day after
 * `my_day` (a friend's clock is allowed to be one day ahead, not more), and
 * `my_day` outside the days the record can hold, which includes
 * OBSERVORE_CENSUS_NO_DAY. Days that fell out of the window before this node's
 * day are not an error and are not stored: they leave the merge SAME.
 *
 * Returns CHANGED only when the stored entry's bytes changed. */
observore_census_merge_t observore_census_peer_merge(uint16_t node, int my_day,
                                                     const observore_census_entry_t *in);

/* How many identities are held from the owner's other nodes. */
size_t observore_census_peer_count(void);

/* Whether the peer table exists. It always does where it is static; where it
 * is allocated at init (OBSERVORE_CX_STATE_IN_PSRAM) it is absent only if that
 * allocation failed, and then the exchange stays off and the census reads as
 * though no other node had reported anything. */
bool observore_census_peer_ready(void);

/* Why the peer table last emptied. */
typedef enum {
    OBSERVORE_CENSUS_FORGOT_NONE = 0,
    OBSERVORE_CENSUS_FORGOT_OWNER,       /* the owner asked */
    OBSERVORE_CENSUS_FORGOT_SECRET,      /* the household secret changed */
    OBSERVORE_CENSUS_FORGOT_NO_SECRET,   /* there is no household secret */
} observore_census_forget_t;

/* Empty the peer table and remember why. This node's own census is not
 * touched. */
void observore_census_peer_forget(observore_census_forget_t why);

/* The last reason the peer table emptied: NONE until something forgot it. A
 * later merge does not clear it, so the console can still say why the table
 * started empty. */
observore_census_forget_t observore_census_peer_forgot(void);

/* Remove the entries none of whose friends has a day left inside the window as
 * of `day`. A friend one day ahead keeps its tomorrow. With no day nothing is
 * known to have aged out, and nothing is removed. */
void observore_census_peer_prune(int day);

/* True once after any change since the last call: a merge that changed an
 * entry, a forget of a table that held something, a prune that removed
 * something, a re-key that emptied the table. The radio path saves the table
 * at the close of a window only when this is true, which keeps the flash
 * writes to one a window. */
bool observore_census_peer_take_changed(void);

/* Serialise the peer table, header and all, into `out`, as
 * observore_census_blob() does for this node's own: the size needed is
 * returned, and with `out` NULL or `cap` too small nothing is written. */
size_t observore_census_peer_blob(void *out, size_t cap);

/* Replace the peer table from a blob previously saved, held under the key
 * check `check` (four bytes of an HMAC of the household secret).
 *
 * Refused, and the table emptied rather than left half read: another format
 * version, a count above the table, a length that is not exactly the header
 * plus that many entries. A table saved under a different key check is refused
 * too, emptied, and the reason recorded as FORGOT_SECRET: what another
 * household secret reported is not this household's. The check is stored with
 * the table either way. Restoring into the store itself skips the copy, as the
 * local restore does. Returns false when the blob was refused. */
bool observore_census_peer_restore(const void *blob, size_t len,
                                   const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN]);

/* The household secret may have changed: hold the table under `check`. Returns
 * true when the stored check differed and a table that held something was
 * emptied, with FORGOT_SECRET recorded. The new check is stored either way. */
bool observore_census_peer_rekey(const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN]);

/* What this node may put on the air: its own census, as of `day`.
 *
 * This is the only way the local census reaches the air. It reads this node's
 * own table and never the peer table, so a friend's days are never relayed and
 * no day can loop between nodes: an identity only a friend has seen is not
 * here, and one this node has seen carries only the days this node saw. Every
 * entry with a day still inside the window is copied with its days aligned to
 * `day` and last_day set to it, which is the form a receiver reads without
 * knowing this node's clock.
 *
 * The table is copied under one lock hold, so a round of frames never mixes two
 * states of it. Returns how many entries were written, at most `max`; 0 when
 * the census is empty, when every day in it has aged out, and for
 * OBSERVORE_CENSUS_NO_DAY or a day the record cannot hold. A node with nothing
 * to say sends nothing. */
size_t observore_census_local_sendable(int day, observore_census_entry_t *out, size_t max);

#ifndef OBSERVORE_HOST_TEST
/* Bring the peer table back from flash at boot, held under `check`, the key
 * check of the household secret now set. Call once, after observore_census_init()
 * (which empties the table and its check) and after the secret has been loaded.
 *
 * With no secret (`have_secret` false) the stored table is not trusted and not
 * kept: whatever flash holds is read only to learn that it is there, the table
 * starts empty with the reason recorded, and the empty table is written back so
 * flash agrees with RAM. With a secret, the stored table is restored when it
 * carries this check; one written by another build or under another secret
 * starts the table empty, says which, and is overwritten at once. Nothing stored
 * is the quiet first-boot case: an empty table, with the check held. */
void observore_census_peer_load(const uint8_t check[OBSERVORE_PEER_CENSUS_CHECK_LEN],
                                bool have_secret);

/* Write the peer table to flash, header and entries as they stand. The radio
 * path calls it at most once a window, when the table changed. A failure is
 * logged and leaves the table in RAM as it was. */
void observore_census_peer_save(void);
#endif

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
