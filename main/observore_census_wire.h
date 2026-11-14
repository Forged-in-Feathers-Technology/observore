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

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
