#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#endif

#include "observore_census.h"

#if CONFIG_OBSERVORE_MESH_CENSUS

/* What a census frame is, and why each part of it is there.
 *
 * A census frame carries some of this node's own census to the owner's other
 * nodes over ESP-NOW. It is a contract between firmware versions: two nodes
 * flashed months apart must read each other's bytes, so the layout below is
 * fixed, versioned, and written field by field. Nothing in this file includes
 * ESP-IDF, so the host suite pins every byte of it.
 *
 *   0-1    magic 'O' 'C'    Observore's census, as opposed to any other
 *                           ESP-NOW traffic on the channel. A neighbour's
 *                           gadget is "foreign" and not a forgery, so the
 *                           rejected count the owner reads does not rise for
 *                           traffic that is not an attack.
 *   2      version          the escape hatch of a one-way contract. A frame
 *                           from a version this code does not know is
 *                           classified and never decrypted.
 *   3      kind             0 is a census chunk; the rest are reserved.
 *   4-5    node id          the sender, as the BLE mesh names it.
 *   6-7    sender's day     the census day on the sender's clock. The day
 *                           check is what stops a wrong clock inventing days.
 *   8-9    window id        random per window, so a receiver counts a friend
 *                           once however many chunks arrive and drops a chunk
 *                           it already has.
 *   10     chunk index      0-based.
 *   11     chunk total      1 to 8.
 *   12     entry count      1 to 8 in this chunk.
 *   13-24  nonce            96 random bits, fresh for every frame.
 *
 * The header is the additional authenticated data of an AES-CCM seal, the
 * entries are the ciphertext, and a full 16-byte tag follows. Only the shape
 * of the header may be read before the tag has verified: a node that believes
 * a header field first lets anyone on the channel choose what it reads next.
 *
 * Entries are written a field at a time, little-endian, because
 * observore_census_entry_t has padding and a compiler is free to lay it out
 * differently from one firmware to the next. Copying the struct onto the air
 * would make the bytes a function of the build rather than of the contract.
 */

/* 'O' and 'C'. */
#define OBSERVORE_CX_MAGIC0 0x4F
#define OBSERVORE_CX_MAGIC1 0x43

/* The first and, so far, only layout. A change to any offset, label or length
 * here is a new version and a transition rule on every node. */
#define OBSERVORE_CX_VERSION 1

/* A census chunk. Other kinds are reserved, and a frame carrying one is
 * malformed rather than ignored, so a later kind cannot be mistaken for this
 * one by a node that does not know it. */
#define OBSERVORE_CX_KIND_CENSUS 0

/* The header, which is also the AAD: magic 2, version 1, kind 1, node 2, day 2,
 * window 2, index 1, total 1, count 1, nonce 12. */
#define OBSERVORE_CX_HDR_LEN 25

/* Where the nonce sits in the header, and how long it is. CCM takes 7 to 13
 * bytes of nonce; twelve is 96 random bits, which is collision-free over a key's
 * lifetime without a persisted counter. A counter that restarted after a flash
 * erase would reuse a nonce under the same key, and CCM does not survive that. */
#define OBSERVORE_CX_NONCE_OFF 13
#define OBSERVORE_CX_NONCE_LEN 12

/* The CCM tag, in full. A shorter tag saves bytes that would not fit another
 * entry and is easier to forge. */
#define OBSERVORE_CX_TAG_LEN 16

/* One entry: id 4, days 2, last_day 2, eight address hashes 16, addr_n 1,
 * addr_over 1. */
#define OBSERVORE_CX_ENTRY_LEN 26

/* Entries in one frame, and frames in one window. Eight of eight is the whole
 * local census (OBSERVORE_CENSUS_MAX), so a window can carry everything this
 * node knows. */
#define OBSERVORE_CX_ENTRIES_MAX 8
#define OBSERVORE_CX_CHUNKS_MAX  8

/* The largest frame: a full header, a full set of entries and the tag. */
#define OBSERVORE_CX_FRAME_MAX \
    (OBSERVORE_CX_HDR_LEN + OBSERVORE_CX_ENTRY_LEN * OBSERVORE_CX_ENTRIES_MAX + \
     OBSERVORE_CX_TAG_LEN)

/* How far apart two nodes' days may be: one. Two nodes either side of local
 * midnight, or a clock set a day out, are a day apart and still honest; two
 * days apart is a clock that is wrong. */
#define OBSERVORE_CX_DAY_SKEW 1

_Static_assert(OBSERVORE_CX_FRAME_MAX <= 250,
               "a census frame fits ESP-NOW v1, which every chip and IDF build reads");
_Static_assert(OBSERVORE_CX_ENTRIES_MAX * OBSERVORE_CX_CHUNKS_MAX >= OBSERVORE_CENSUS_MAX,
               "one window can carry the whole local census");
_Static_assert(OBSERVORE_CX_NONCE_OFF + OBSERVORE_CX_NONCE_LEN == OBSERVORE_CX_HDR_LEN,
               "the nonce is the end of the header");

/* The header's fields as a caller sees them. The magic, version and kind are
 * not here: the packer writes them and the parser checks them, so no caller can
 * put a wrong one on the air. */
typedef struct observore_cx_hdr_t {
    uint16_t node;
    uint16_t day;
    uint16_t window;
    uint8_t  index;
    uint8_t  total;
    uint8_t  count;
    uint8_t  nonce[OBSERVORE_CX_NONCE_LEN];
} observore_cx_hdr_t;

/* What a frame looks like before anything in it is believed. */
typedef enum observore_cx_frame_t {
    OBSERVORE_CX_FRAME_OK = 0,
    OBSERVORE_CX_FRAME_FOREIGN,    /* not Observore's: wrong magic, or too short to say */
    OBSERVORE_CX_FRAME_VERSION,    /* Observore's, from a version this code does not read */
    OBSERVORE_CX_FRAME_MALFORMED,  /* the right version with a shape that cannot be one */
} observore_cx_frame_t;

/* Write the header into `out`, which must hold OBSERVORE_CX_HDR_LEN bytes, and
 * return that length. Magic, version and kind are written here and never by
 * the caller. */
size_t observore_cx_put_hdr(const observore_cx_hdr_t *h, uint8_t *out);

/* Write one entry into `out`, which must hold OBSERVORE_CX_ENTRY_LEN bytes, and
 * return that length. Address hashes past addr_n are written as zero, so the
 * same census always packs to the same bytes whatever the table held. */
size_t observore_cx_put_entry(const observore_census_entry_t *e, uint8_t *out);

/* Classify a frame by its shape alone: the magic and the length first
 * (FOREIGN), then the version (VERSION), then the kind, the counts, the index
 * and the exact length the counts imply (MALFORMED).
 *
 * Nothing this returns may be believed until the tag has verified. `hdr`, which
 * may be NULL, is filled only for an OK frame, and only so the caller can use
 * node, day and window after verification: until then every one of them is
 * whatever the sender chose to write. */
observore_cx_frame_t observore_cx_check_frame(const uint8_t *frame, size_t len,
                                              observore_cx_hdr_t *hdr);

/* Read one entry from OBSERVORE_CX_ENTRY_LEN bytes of decrypted frame, as sent
 * on `sender_day`. Call it only on bytes whose tag has verified.
 *
 * Refused, and `out` left unwritten: id 0; an address count above the ceiling;
 * an over flag other than 0 or 1; a last_day after the sender's day; a last_day
 * a whole window or more before it; and a day mask with nothing left inside the
 * window at the sender's day. A refusal is a classification and never stored
 * data. */
bool observore_cx_get_entry(const uint8_t *in, int sender_day,
                            observore_census_entry_t *out);

/* Whether two nodes' days agree well enough to believe the sender's. A wrong
 * clock on either side could otherwise invent days or move them, and one day of
 * tolerance covers two nodes either side of midnight. A node with no day, a
 * negative one, or one past what the record can hold agrees with nobody. */
bool observore_cx_day_ok(int sender_day, int my_day);

/* ---------------------------------------------------------------------
 * The window: what is sent, who was heard, when it opens and when it closes.
 *
 * A window is a short stretch in which this node listens for the owner's other
 * nodes, and in exchange mode sends its own census. Everything a window has to
 * decide is a pure function below, so the radio path that drives it has nothing
 * left to decide about bytes or time, and the suite pins every bound.
 */

/* Split `n` entries into frames of OBSERVORE_CX_ENTRIES_MAX, at most
 * OBSERVORE_CX_CHUNKS_MAX of them. Nothing to send is zero frames, which is
 * how a node with nothing to say stays silent. */
uint8_t observore_cx_plan(size_t n);

/* Where chunk `index` of `n` entries starts and how many entries it holds. An
 * index past the last chunk is empty. */
void observore_cx_chunk(size_t n, uint8_t index, size_t *first, uint8_t *count);

/* How many friends one window keeps track of. A household has a handful of
 * nodes; a ninth is counted and ignored, which bounds the table that a
 * stranger's flood of valid-looking frames could otherwise fill. */
#define OBSERVORE_CX_FRIENDS_MAX 8

/* Who has been heard in this window, in the order they were first heard, and
 * which of each one's chunks have arrived. `chunks` is a bit per chunk index.
 * `crowded` counts frames from a ninth and later node. */
typedef struct observore_cx_heard_t {
    uint8_t  n;
    uint16_t crowded;
    struct {
        uint16_t node;
        uint16_t window;
        uint8_t  chunks;
    } f[OBSERVORE_CX_FRIENDS_MAX];
} observore_cx_heard_t;

typedef enum observore_cx_seen_t {
    OBSERVORE_CX_NEW = 0,     /* a chunk not yet held */
    OBSERVORE_CX_DUPLICATE,   /* held already, or not a chunk index at all */
    OBSERVORE_CX_CROWDED,     /* a ninth friend: counted, not kept */
} observore_cx_seen_t;

/* Note one verified chunk. A friend is kept once however many chunks it sends;
 * a known friend with a new window id starts again, since a new window is a
 * new set of chunks, and stays one friend. Call it only for frames whose tag
 * has verified: this table is where a repeated frame stops being news. */
observore_cx_seen_t observore_cx_heard_note(observore_cx_heard_t *h, uint16_t node,
                                            uint16_t window, uint8_t index);

/* The three things the owner can set a node to. Off says nothing and hears
 * nothing; listen hears and never sends; exchange does both. */
#define OBSERVORE_CX_OFF      0
#define OBSERVORE_CX_LISTEN   1
#define OBSERVORE_CX_EXCHANGE 2

/* "off", "listen", "exchange", and "?" for anything else. */
const char *observore_cx_mode_name(uint8_t mode);

/* Exactly one of those three words. `mode` is untouched on failure. */
bool observore_cx_mode_parse(const char *s, uint8_t *mode);

/* A time of day, exactly HH:MM from 00:00 to 23:59, as minutes since midnight.
 * `minute_of_day` is untouched on failure. */
bool observore_cx_at_parse(const char *s, uint16_t *minute_of_day);

/* Whether `ch` may be the household channel: 1 to 13. That is a 2.4 GHz channel
 * every chip and regulatory domain here allows, which also keeps a C5 off 5 GHz
 * for the window. */
bool observore_cx_channel_ok(int ch);

/* How long a window may last, and no longer. Patrol is paused while one is open,
 * and a detector that stops looking is one a stranger could use, so the bound is
 * a number the compiler holds the code to. */
#define OBSERVORE_CX_WINDOW_MAX_S 30

_Static_assert(OBSERVORE_CX_WINDOW_MAX_S <= 30,
               "patrol pauses for at most thirty seconds a window");

/* How late after the target second a scheduled window may still open. A patrol
 * cycle is about eight seconds, so two nodes whose clocks agree open within
 * about ten seconds of each other; twenty seconds of grace lets a node whose
 * loop was busy at the target still take part. */
#define OBSERVORE_CX_GRACE_S 20

/* The least time between a window closing and a hand-opened one starting.
 *
 * The same principle as the uplink window's hard ceiling: a user interface must
 * not be able to blind the detector. Without a gap a person, or a script on the
 * household network, could keep asking for a window and hold patrol off for as
 * long as it liked. Five minutes leaves patrol more than ninety per cent of the
 * time even then. */
#define OBSERVORE_CX_MIN_GAP_S 300

/* The default time of the daily window, 03:30 in minutes since midnight. It is
 * outside the 02:00 to 03:00 hour that daylight saving skips or repeats in the
 * US and EU; where a zone does shift at 03:30, the once-per-local-day rule loses
 * or keeps one window, which is harmless. */
#define OBSERVORE_CX_AT_DEFAULT 210

/* Everything the decision to open a window reads, gathered by the caller so
 * the decision itself touches no clock.
 *
 *   clock_valid, today, sod   whether the clock is real, the census day, and
 *                             seconds since local midnight.
 *   at_min                    the owner's window time, minutes since midnight.
 *   last_day                  the census day a scheduled window last ran on.
 *   now_us, last_close_us     the monotonic time now and when the last window
 *                             closed; 0 when none has this boot.
 *   mode, have_secret         what the owner set, and whether there is a
 *                             household secret to derive the key from.
 *   manual                    the owner asked for a window now. */
typedef struct observore_cx_when_t {
    bool     clock_valid;
    int      today;
    int      sod;
    uint16_t at_min;
    int      last_day;
    int64_t  now_us;
    int64_t  last_close_us;
    uint8_t  mode;
    bool     have_secret;
    bool     manual;
} observore_cx_when_t;

typedef enum observore_cx_decision_t {
    OBSERVORE_CX_WAIT = 0,         /* nothing to do yet */
    OBSERVORE_CX_OPEN_SCHEDULED,   /* open, and mark today's window as run */
    OBSERVORE_CX_OPEN_NOW,         /* open, and leave today's window unmarked */
    OBSERVORE_CX_SKIP_OFF,
    OBSERVORE_CX_SKIP_NO_SECRET,
    OBSERVORE_CX_SKIP_NO_CLOCK,
    OBSERVORE_CX_SKIP_TOO_SOON,
} observore_cx_decision_t;

/* Whether a window opens now, and why not when it does not.
 *
 * In this order: off refuses everything, then no household secret, then no clock
 * (a manual window included: a window with no day has nothing to send and no
 * day check to pass). Then a manual window opens unless the last window closed
 * less than OBSERVORE_CX_MIN_GAP_S ago. Then a scheduled window waits if it has
 * already run on this census day, and opens when the second of the day is inside
 * [at, at + OBSERVORE_CX_GRACE_S).
 *
 * The gap applies to manual windows only, because a scheduled window already
 * runs at most once a local day. A manual window does not consume the day's
 * scheduled one: the caller marks the day only for OPEN_SCHEDULED. */
observore_cx_decision_t observore_cx_decide(const observore_cx_when_t *w);

/* True at or past OBSERVORE_CX_WINDOW_MAX_S seconds after `opened_us`. */
bool observore_cx_should_close(int64_t opened_us, int64_t now_us);

/* Whole seconds left in the window, rounded up, so it reads 1 in the last
 * second and 0 only once the window is closed. */
unsigned observore_cx_remaining_s(int64_t opened_us, int64_t now_us);

/* Seconds until the next scheduled window may open: -1 when there will be none
 * (off, no secret or no clock), 0 inside the grace when it has not yet run
 * today, otherwise the time to the window time today, or tomorrow when today's
 * has run or passed. */
long observore_cx_seconds_until(const observore_cx_when_t *w);

/* What one window did, as counts only. Nothing from a refused frame is kept,
 * not even the sender's address: a stranger's frame leaves a number behind and
 * nothing else.
 *
 *   mode, interrupted, secs   what ran, whether it was cut short, and for how
 *                             long.
 *   sent, rounds              entries and frame rounds this node sent.
 *   friends, friend_node      who was heard, in the order first heard.
 *   merged                    entries merged into the peer table.
 *   bad_tag, day_skew         frames that failed authentication, and frames
 *                             whose day was more than a day from ours.
 *   malformed, foreign        frames of ours with an impossible shape, and
 *                             other ESP-NOW traffic.
 *   dropped, crowded          chunks held already, and frames from a ninth
 *                             friend.
 *   entries_bad               entries refused inside a verified frame.
 *   left_mode, day, closed_us what the node went back to, the census day, and
 *                             when the window closed. */
typedef struct observore_cx_result_t {
    uint8_t  mode;
    bool     interrupted;
    uint8_t  secs;
    uint16_t sent;
    uint8_t  rounds;
    uint8_t  friends;
    uint16_t friend_node[OBSERVORE_CX_FRIENDS_MAX];
    uint16_t merged, bad_tag, day_skew, malformed, foreign, dropped, crowded, entries_bad;
    uint8_t  left_mode;
    int      day;
    int64_t  closed_us;
} observore_cx_result_t;

/* Room for the longest text observore_cx_result_text() can write, with its
 * terminator. */
#define OBSERVORE_CX_RESULT_TEXT_LEN 224

/* One line saying what a window did, counts and friend ids only, for the
 * console, the card and the log. `share` is what the owner calls exchange.
 * snprintf semantics: the text is clamped to len - 1 and always terminated, and
 * the return value is the length written. */
size_t observore_cx_result_text(const observore_cx_result_t *r, char *buf, size_t len);

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
