#pragma once

#include <stddef.h>

/* Writing a CSV field that cannot break the file.
 *
 * Nearly every string in a detection came out of the air: an SSID, a
 * Bluetooth advertised name, a vendor string matched from an address. A
 * network called `Cafe, Free WiFi` or `Bob"s iPhone` is not unusual, and
 * neither is one with a newline in it -- an SSID is 32 arbitrary bytes and
 * nothing stops an attacker choosing them. Pasted into a CSV unescaped, the
 * first of those adds a column, the second breaks quoting for the rest of
 * the line, and the third ends the record early.
 *
 * A detector whose log can be reshaped by naming a network after a comma is
 * one whose log cannot be trusted, which matters more here than it would in
 * most places: this file is the evidence.
 *
 * So RFC 4180: wrap in quotes, double any quote inside. Control characters
 * are dropped rather than escaped, because a bare newline inside a quoted
 * field is legal CSV that half the tools in the world mishandle, and
 * nothing is lost that a reader of this file wanted.
 *
 * Here, separate from the writer, so the host suite can throw the awkward
 * strings at it without a card in the slot.
 */

/* Write `in` into `out` as one quoted CSV field, always quoted so the rules
 * are the same for every field and there is nothing to get wrong per case.
 * Truncates to fit rather than overflowing, and always terminates. Returns
 * the length written, not counting the terminator. A NULL `in` is an empty
 * field. */
size_t observore_csv_field(char *out, size_t cap, const char *in);
