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
 *   6..9   node id, little-endian
 *   10     the class being warned about (observore_class_t)
 *   11..12 sequence, little-endian
 *   13..14 how many seconds ago the sender saw it, little-endian
 *   15..18 latitude  e7, if bit 0
 *   19..22 longitude e7, if bit 0
 *   ...    a four-byte tag, if bit 1
 *
 * A legacy advert carries 31 bytes, of which the manufacturer payload gets 27
 * -- 24 once a flags element is included. So fifteen bytes of warning fits
 * comfortably, a warning with a position fits at twenty-three, and a warning
 * that is both positioned and signed does not: it needs BLE 5 extended
 * advertising, which every board here except the classic ESP32 CYDs supports.
 * That is a real constraint of the format and is written down rather than
 * discovered later.
 *
 * ## What is deliberately not here yet
 *
 * No key management, so nothing can be trusted and `trusted` is always false.
 * The field exists because the parser has to know a tag may follow in order
 * to not mistake it for payload, and because the format should not need
 * changing when keys arrive.
 */

#define OBSERVORE_PEER_MAGIC   "OBW1"
#define OBSERVORE_PEER_VERSION 1

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
    uint32_t node;       /* who said it */
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
