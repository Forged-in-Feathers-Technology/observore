#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#endif

/* The console's own certificate, and the fingerprint that makes it mean
 * something (#11).
 *
 * The console is plain HTTP. Authentication landed in #10, which stops casual
 * access; it does nothing about a listener, and the session cookie crosses the
 * LAN in the clear. TLS is the other half.
 *
 * ## What the threat actually is
 *
 * The issue said the log "can be read by anything in range holding the network
 * passphrase". On WPA2-PSK that is not quite true, and the difference decides
 * what this certificate has to achieve. Each station derives its own pairwise
 * key from its own four-way handshake, so another station that knows the
 * passphrase cannot passively decrypt somebody else's unicast traffic -- it
 * has to capture that handshake first, or ARP-spoof its way into the path.
 * Group-addressed traffic is readable by every station, because the group key
 * is shared, but the console's responses are unicast.
 *
 * Forcing a handshake to be re-captured means a deauthentication, which this
 * device detects and scores at six. So the realistic attacker here is an
 * active one already on the network, not a bystander within radio range.
 *
 * That matters because an active attacker is exactly the case plain encryption
 * does not cover. Encryption without authentication stops the bystander who
 * was mostly not there anyway, and loses to the attacker who is.
 *
 * ## Why not simply self-signed and click through the warning
 *
 * Because the warning is the whole security of the connection, and a warning
 * that is clicked through every visit has no security in it. Worse, it looks
 * identical whether or not somebody is intercepting: a browser cannot tell a
 * device's own certificate from an attacker's, so the owner of a
 * counter-surveillance device would be trained to dismiss the one signal that
 * would have told them. That is a bad trade dressed as a padlock.
 *
 * ## What this project has that most devices do not: a screen
 *
 * Five of the nine boards have one. So the certificate is self-signed and
 * generated on the device, and its fingerprint is shown somewhere an attacker
 * on the network cannot reach -- on the glass, and on the serial console.
 * Compare it once, the way an SSH host key is compared, and the warning stops
 * being a habit and becomes a check with a known answer.
 *
 * Headless boards print it on serial at boot, which is the same cable the
 * console password already needs, and serve it from the API once trusted.
 *
 * This is trust on first use. It is not a CA, it does not survive an attacker
 * who is in the path the very first time, and it is worth saying so plainly
 * rather than implying more. What it does buy is that an interception after
 * that first visit changes the fingerprint, and the device will still be
 * showing the real one on its own screen.
 *
 * ## Why the screen shows ten bytes and not all thirty-two
 *
 * A full SHA-256 is sixty-four hex characters. On a 320-pixel panel that is
 * either unreadable or four lines of hex nobody will actually compare, and a
 * comparison nobody performs is worth less than a shorter one they do.
 *
 * Ten bytes is eighty bits. For this to be defeated an attacker must produce a
 * certificate they hold the key to whose fingerprint shares those eighty bits
 * -- a second-preimage search, not a birthday collision, because the target is
 * a fingerprint that already exists. That is 2^80 work for an attacker whose
 * goal is reading one household's detection log, and the full value is on
 * serial and in the API for anyone who wants to check all of it.
 *
 * The format is the one browsers show, so the comparison is character for
 * character and does not require anybody to re-encode anything in their head.
 *
 * ## Why the hash is of the DER, not the PEM
 *
 * Because that is what browsers call the SHA-256 fingerprint. Hashing the PEM
 * would produce a number that is correct, stable, reproducible, and matches
 * nothing the owner can see in their own browser -- which makes it useless for
 * the one purpose it has.
 *
 * ## Why P-256 and not RSA
 *
 * Key generation happens on the device, once, during first boot. An RSA-2048
 * keypair costs seconds to minutes on these parts and the time varies with
 * luck, because it is searching for primes. A P-256 key is one scalar
 * multiplication. Browsers have accepted ECDSA certificates for years.
 *
 * ## Where it is not offered
 *
 * Not on the SoftAP. During first-time setup there is no name to put in a
 * certificate, no screen on a headless board to show a fingerprint against,
 * and WPA2 already protects that link with a password only the owner has. The
 * SoftAP stays HTTP deliberately.
 *
 * Not where the internal heap cannot carry it. Serving a handshake costs more
 * than making one, and the measurements in docs/memory.md are unambiguous:
 * boards with PSRAM reach a low-water mark of about 10 KB under an outbound
 * handshake, while the classic-ESP32 displays reach 820 to 932 bytes. This
 * module therefore reports whether it can, and the caller serves HTTP when it
 * cannot -- a console that answers is worth more than one that was supposed to
 * be encrypted and is not there.
 */

/* SHA-256 of the DER certificate: thirty-two bytes. */
#define OBSERVORE_FP_BYTES 32

/* "AB:CD:...:EF" -- 32 pairs, 31 colons, NUL. The form browsers show. */
#define OBSERVORE_FP_HEX_LEN (OBSERVORE_FP_BYTES * 3)

/* The first ten bytes as five space-separated groups of four hex characters,
 * for a panel: "A1B2 C3D4 E5F6 0718 99AA". Twenty-four characters, NUL. */
#define OBSERVORE_FP_SHORT_BYTES 10
#define OBSERVORE_FP_SHORT_LEN   (OBSERVORE_FP_SHORT_BYTES * 2 + 4 + 1)

/* Format a fingerprint. Both write nothing and return false when the buffer is
 * too small, rather than writing a truncated fingerprint -- a fingerprint that
 * is almost right is the one failure mode this whole mechanism exists to make
 * visible, so it must never be produced by the device itself. */
bool observore_fp_format(const uint8_t *fp, char *out, size_t out_len);
bool observore_fp_short(const uint8_t *fp, char *out, size_t out_len);

/* The rest of this module exists only where the option is on. The pure
 * formatters above are always available, because the host tests exercise them
 * and they cost nothing.
 *
 * This is not tidiness. The four statics below the line measure 1,569 bytes of
 * .bss, and the classic-ESP32 display boards reach a low-water mark of 820
 * bytes of free internal heap -- so compiling this into a board that cannot
 * serve TLS would spend more RAM than those boards have spare, on a feature
 * they are documented as not having. */
#if CONFIG_OBSERVORE_CONSOLE_HTTPS

/* Load the certificate, or generate and persist one on first boot. Call once,
 * before the radio starts -- that is when the most internal heap is free, and
 * generation is the one expensive thing this module does. */
void observore_httpscert_init(void);

/* Whether a usable certificate and key are in hand. */
bool observore_httpscert_present(void);

/* The PEM certificate and key, with lengths including the terminator, which is
 * the form esp_https_server wants. False when there is none. */
bool observore_httpscert_get(const char **cert, size_t *cert_len,
                             const char **key, size_t *key_len);

/* The SHA-256 of the DER certificate: OBSERVORE_FP_BYTES bytes. */
bool observore_httpscert_fp(uint8_t *out);

/* Whether there is enough free internal heap to serve TLS, and how much there
 * is either way so the caller can say why it did not. A refusal here is not a
 * fault: the caller serves plain HTTP and the console still answers. */
bool observore_httpscert_can_serve(uint32_t *free_heap);

#endif /* CONFIG_OBSERVORE_CONSOLE_HTTPS */
