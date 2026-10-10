#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "observore_peer.h"

/* The household secret, and the tag it makes.
 *
 * One key shared by the nodes you own (#132). A warning carrying a tag this
 * key verifies is `trusted`; everything else is a warning from a stranger,
 * which is still heard and still reported. That asymmetry is the whole design
 * and it bounds what this is for: a stranger may warn and nothing may
 * silence, so forging a tag buys more weight on a warning, never blindness.
 *
 * ## Why one shared secret and not a keypair each
 *
 * Per-node keys would need a pairing step for every pair of boards and a key
 * id inside a four-byte field that is already the thing holding a signed,
 * positioned warning inside a 31-byte legacy advert. A household secret needs
 * no pairing, and revocation is "change the key on your nodes", which for a
 * handful of boards is a real answer rather than a shrug.
 *
 * The cost is stated rather than hidden: one compromised node compromises the
 * tier, because every node holds the same secret. This does not defend
 * against somebody who has the key.
 *
 * ## The key never leaves
 *
 * Set through the console, stored in NVS, and never read back -- the same
 * arrangement as the notifier tokens. The console reports only whether a key
 * is present. Nothing serves it, because a key served over the LAN is a key
 * on the LAN, and until #11 that LAN is plain HTTP.
 *
 * Supplied rather than generated here, deliberately. Generating one would
 * mean showing it to somebody, and the only safe place this project shows a
 * secret is the serial log -- which is where the console password goes and
 * why it needs a cable. Taking a key the owner already has avoids inventing a
 * second such ritual.
 *
 * ## Why a 32-bit tag is enough, and what would make it not enough
 *
 * Truncated HMAC-SHA256. Thirty-two bits means a blind forgery succeeds about
 * once in four billion, and a receiver's replay check already refuses a
 * sequence that does not advance -- so an attacker cannot grind against a
 * live node. What they *can* do is capture tagged warnings and brute-force a
 * weak key offline, because a four-byte tag confirms a guess. So the key
 * wants to be long and random, not a passphrase somebody chose. The console
 * says so where it is entered.
 */

/* Thirty-two bytes, entered as 64 hex characters. */
#define OBSERVORE_MESHKEY_BYTES 32
#define OBSERVORE_MESHKEY_HEX   (OBSERVORE_MESHKEY_BYTES * 2)

void observore_meshkey_init(void);

/* Whether a key is set. The only thing about the key anything may report. */
bool observore_meshkey_present(void);

/* Store a key from 64 hex characters, or clear it with NULL or an empty
 * string. False if the text is not a whole key in hex -- a half-parsed key
 * would be a key nobody chose. */
bool observore_meshkey_set(const char *hex);

/* The tag for a message, or false when no key is set.
 *
 * `out` is OBSERVORE_PEER_TAG_LEN bytes. The message is the span before the
 * tag -- see observore_peer_tag_span(). */
bool observore_meshkey_tag(const uint8_t *msg, size_t len, uint8_t *out);

/* Whether `tag` is this key's tag for `msg`. False when no key is set, so a
 * node with no key trusts nothing rather than everything. */
bool observore_meshkey_verify(const uint8_t *msg, size_t len,
                              const uint8_t *tag);
