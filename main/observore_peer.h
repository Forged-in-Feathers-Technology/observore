#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Warnings from other Observore nodes.
 *
 * This half only listens (#132). The device already scans BLE continuously,
 * so hearing a neighbour costs nothing and adds no exposure -- which is the
 * whole reason receiving comes first. Transmitting is a separate, opt-in
 * change, because a node that broadcasts becomes findable by
 * direction-finding, and "there is an observer here" is the one thing a
 * counter-surveillance device should not announce.
 *
 * A receive-only node is therefore a first-class configuration rather than a
 * degraded one: it benefits from every warning in range and emits nothing.
 *
 * ## Strangers may warn; only friends may silence
 *
 * A warning heard from an unknown node can raise attention and can never
 * quiet anything. That asymmetry is what makes an open mesh survivable: an
 * adversary standing up ten fake nodes can produce noise, never blindness,
 * and noise is recoverable in a way that silent suppression is not.
 *
 * In this slice it holds by construction, because nothing here suppresses
 * anything and a warning contributes **zero** to the score. What it does is
 * get reported, which is the honest first step: the device says what it
 * heard and whose it was, and a person decides.
 *
 * ## The wire format
 *
 * Manufacturer-specific data under company 0xFFFF, the SIG's reserved
 * non-production ID, with four magic bytes that make it specific -- the same
 * arrangement SquachWatch uses, and for the same reason: the company ID alone
 * means nothing because every hobby project made the same honest choice.
 * "OBW1" is deliberately not "SQM1".
 *
 *   0..3   magic "OBW1"
 *   4      format version
 *   5      flags: bit 0 a position follows, bit 1 a tag follows
 *   6..7   node id, little-endian
 *   8      the class being warned about (observore_class_t)
 *   9..10  sequence, little-endian
 *   11     how many seconds ago the sender saw it
 *   12..14 latitude,  coarse, if bit 0
 *   15..17 longitude, coarse, if bit 0
 *   18..21 a four-byte tag, if bit 1
 *
 * ## Why every field is as small as it is
 *
 * A legacy advert carries 31 bytes. Four go to the element header and the
 * company ID and three more to a flags element, leaving 24 for the warning.
 * Version 1 spent 23 of them on a positioned warning, which fit with a single
 * byte to spare and left no room at all for a signature -- a signed,
 * positioned warning came to 27 and would have needed BLE 5 extended
 * advertising.
 *
 * That is not a tolerable place to end up, because the classic ESP32 in both
 * Cheap Yellow Displays is BLE 4.2 and cannot *receive* extended advertising
 * at all. Moving the format there would not merely stop those boards
 * transmitting; it would make them deaf to the mesh.
 *
 * The alternative was to buy a Bluetooth SIG company identifier, which would
 * have retired the four magic bytes -- $1,250, for four bytes. So the fields
 * were made honest instead:
 *
 *   The node id is two bytes. Eight nodes in BLE range makes sixteen bits
 *   ample, and a collision costs one node's warnings being merged with
 *   another's rather than anything unsafe.
 *
 *   The age is one byte. Warnings expire at five minutes, so a value that
 *   could express more than 255 seconds was describing a warning that would
 *   already have been dropped.
 *
 *   Positions are three bytes each rather than four, which is the saving
 *   that was wanted anyway: coarse was always the stated preference, since
 *   broadcasting a detector's exact position is its own hazard.
 *
 * Twelve bytes bare, eighteen with a position, twenty-two signed and
 * positioned -- which leaves two spare in a legacy advert, so every board
 * here can hear a signed warning that says where.
 *
 * ## Coarse positions
 *
 * The wire carries degrees times ten million divided by 256, in three signed
 * bytes. That is about 2.8 metres, against the eleven metres the four decimal
 * places already shown to a person imply, so nothing visible is lost. The
 * divisor is a power of two so the arithmetic is exact in both directions
 * and needs no rounding decision.
 *
 * ## What is deliberately not here yet
 *
 * No key management, so nothing can be trusted and `trusted` is always false.
 * The field exists because the parser has to know a tag may follow in order
 * to not mistake it for payload, and because the format should not need
 * changing when keys arrive.
 */

#define OBSERVORE_PEER_MAGIC   "OBW1"
/* Bumped from 1 when the fields were tightened. A version this build does
 * not know is refused rather than guessed at, so an old node and a new one
 * simply do not hear each other -- which is the right failure, and cheap
 * right now because nothing transmits yet. */
#define OBSERVORE_PEER_VERSION 2

/* Warnings remembered, and for how long.
 *
 * Eight is generous for anything in BLE range and bounded enough that a
 * crowd of senders cannot push the table around. Five minutes because a
 * warning is about now: one older than that describes a situation that has
 * moved, and keeping it would let a single sighting look like a standing
 * alarm. */
#define OBSERVORE_PEER_MAX      8
#define OBSERVORE_PEER_TTL_S  300

/* The sequence window.
 *
 * A warning whose sequence does not advance is a replay and is dropped. The
 * comparison is modular rather than arithmetic, so a sender that wraps from
 * 65535 to 0 -- or reboots and starts again -- is still heard: anything
 * within half the space ahead counts as newer. The alternative, a plain
 * greater-than, silences a node permanently the first time it wraps. */
#define OBSERVORE_PEER_SEQ_WINDOW 32767

typedef struct {
    uint16_t node;       /* who said it */
    uint8_t  cls;        /* what they saw */
    uint16_t seq;        /* their counter */
    uint16_t age_s;      /* how long before sending they saw it */
    bool     have_pos;
    int32_t  lat_e7, lon_e7;
    bool     trusted;    /* carried a tag this device could verify: never yet */
    int64_t  heard_us;   /* when we heard it, for ageing */
} observore_peer_warning_t;

/* Decode manufacturer payload, which begins at the magic -- the caller has
 * already matched company 0xFFFF and stripped it.
 *
 * A version this build does not know is refused rather than guessed at.
 * Unlike another project's format, this one is ours: reading fields by a
 * layout we have since changed is how a position ends up in the wrong place.
 * The caller can still tell it heard an Observore node, which is a separate
 * question from being able to read what it said.
 *
 * `heard_us` is left for the caller to fill. Returns false on anything it
 * cannot read whole. */
bool observore_peer_parse(const uint8_t *payload, size_t len,
                          observore_peer_warning_t *out);

/* The same thing from a whole BLE advert: find the manufacturer element,
 * check the company, and decode.
 *
 * Here rather than at the call site so that "which element, which company,
 * which magic" is written once. The BLE layer should not have to know the
 * format in order to hand it over, and the duplicate that the first draft of
 * this had -- an AD type constant copied out of the classifier -- is exactly
 * the sort of thing that drifts. */
bool observore_peer_from_advert(const uint8_t *adv, size_t adv_len,
                               observore_peer_warning_t *out);

/* This node's id, from its own address.
 *
 * Sixteen bits, and coarse on purpose. Over the handful of nodes that can be
 * in BLE range of each other a collision is unlikely, and when it happens two
 * nodes' warnings merge into one entry -- a little precision lost and nothing
 * unsafe. It is also not an identity: the address on the air is random and
 * regenerated for every burst, so this is what a receiver can group warnings
 * by and nothing more.
 *
 * Stable across reboots, because the replay check on the receiving side keys
 * on the node and would treat a node that renamed itself every boot as a new
 * neighbour each time.
 *
 * Takes the address rather than reading it, so it stays pure and the host
 * tests can check that it is stable and that it never returns zero -- zero
 * being a value a receiver would see as a node that had not set one. */
uint16_t observore_peer_node_id(const uint8_t mac[6]);

/* Build a warning payload, from the magic to the last optional field.
 *
 * The inverse of observore_peer_parse(), and tested against it: a warning
 * built here and read back there has to come out with the same fields. That
 * round trip is the only check that actually pins the wire format, because
 * every other test of the parser feeds it bytes a person typed.
 *
 * `w->trusted` is ignored. There is no key management, so nothing here can
 * sign anything, and emitting a tag this device cannot produce would invite a
 * receiver to believe it. The flag exists so the format need not change when
 * keys arrive.
 *
 * Returns the length written, or 0 if it would not fit. */
size_t observore_peer_build(const observore_peer_warning_t *w,
                            uint8_t *out, size_t cap);

/* The same thing as a complete manufacturer-specific AD element, ready to
 * hand to the BLE stack: length, type, company, then the payload.
 *
 * Here rather than at the call site for the same reason the matching decoder
 * is: "which element, which company, which magic" is written once, and the
 * radio layer should not have to know the format in order to send it. */
size_t observore_peer_advert(const observore_peer_warning_t *w,
                             uint8_t *out, size_t cap);

void observore_peer_init(void);

/* Record a warning, or drop it as a replay or a duplicate. Returns true when
 * it was new and has been kept. */
bool observore_peer_note(const observore_peer_warning_t *w, int64_t now_us);

/* Warnings heard recently, newest first, excluding anything aged out. */
size_t observore_peer_recent(observore_peer_warning_t *out, size_t max,
                             int64_t now_us);

/* How many distinct nodes have warned recently, and how many warnings stand.
 * Either pointer may be NULL. */
void observore_peer_counts(int64_t now_us, int *nodes, int *warnings);
