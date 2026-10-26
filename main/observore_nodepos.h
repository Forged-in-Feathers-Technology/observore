#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Where this node is, if its owner has told it.
 *
 * A warning carrying a position is far more use than one without: "something
 * was seen" becomes "something was seen there". The wire format has carried
 * one since #160 and the receiver has decoded it since, but nothing ever
 * filled it in, so every warning this project has ever sent has been
 * positionless. This is the thing that fills it.
 *
 * ## Why the owner rather than a GPS
 *
 * A stationary node knows where it is the moment somebody tells it, and most
 * nodes are stationary -- that is the whole premise of learning what is
 * normally around. A GPS is the answer for a node that moves, costs a part
 * and a UART, and is fitted to exactly one board profile. So this comes
 * first, and reading a position out of NMEA is a second source later, not a
 * prerequisite.
 *
 * ## Deliberately absent unless set
 *
 * There is no default and no guess. Broadcasting where a detector is sitting
 * is its own hazard, and a device that invented a position -- from a
 * timezone, from an IP geolocation, from anything -- would be making that
 * decision on the owner's behalf. Unset means warnings go out exactly as
 * they did before.
 *
 * It is also only ever transmitted by a node that is already transmitting:
 * mesh broadcast is off by default and off means the advertising code is not
 * compiled in. Setting a position on a silent node changes nothing about
 * what leaves it.
 *
 * ## Coarse on the wire regardless
 *
 * Stored exactly, sent divided by 256 -- about 2.8 metres. See the coarse
 * position note in observore_peer.h. There is no setting for this: a node
 * that wanted to broadcast its exact location would be asking for a
 * precision that only helps somebody looking for it.
 */

void observore_nodepos_init(void);

/* The stored position. False when none is set, and then the outputs are
 * untouched. */
bool observore_nodepos_get(int32_t *lat_e7, int32_t *lon_e7);

/* Store one, in degrees times ten million. Rejects anything
 * observore_peer_position_usable() would refuse, at the point a person typed
 * it rather than silently at transmit time. */
bool observore_nodepos_set(int32_t lat_e7, int32_t lon_e7);

/* Forget it. Warnings go back to being positionless. */
void observore_nodepos_clear(void);
