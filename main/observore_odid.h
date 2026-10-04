#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reading what a drone broadcasts about itself.
 *
 * ASTM F3411 / OpenDroneID. A compliant drone transmits, in clear and
 * unauthenticated, its own position, its altitude, its speed and heading, its
 * serial number, and -- the part that surprises people -- the location of its
 * operator. Regulation requires it, which is why it is there to be read.
 *
 * The device already recognised these broadcasts and labelled them `drone`.
 * It threw the payload away. One node that decodes it knows where the drone
 * is and where the pilot is standing, which is worth more than any number of
 * nodes reporting that they saw something (#132).
 *
 * ## What this is not
 *
 * **Silence here is not evidence of no drone.** Consumer aircraft broadcast
 * Remote ID because they are obliged to. Anything that does not want to be
 * found does not transmit it, and this radio sees only 2.4 GHz Wi-Fi and BLE
 * in any case. A quiet screen means "nothing announced itself", never "the
 * sky is clear", and anything built on top of this has to say so where it
 * cannot be missed.
 *
 * ## Byte arithmetic rather than packed structs
 *
 * The reference implementation describes the wire format as packed bitfield
 * structs. Those are laid out at the compiler's discretion -- bitfield
 * ordering within a unit is implementation-defined, and this builds for four
 * different targets -- so the fields are pulled out by explicit shifts and
 * little-endian loads instead. It also means the whole decoder is pure and
 * the host tests can drive it with captured bytes.
 *
 * Offsets were taken from the reference header rather than from memory, after
 * a first draft placed latitude at bytes 4-7. It is at 5-8: a one-byte error
 * that yields coordinates which look entirely plausible and are wrong by
 * continents.
 */

/* Position is kept in the units the wire uses -- degrees times ten million --
 * rather than converted to floating point. It is exact, it costs nothing on a
 * chip with no FPU to spare, and formatting belongs at the edge. */
typedef struct {
    bool    have_location;   /* a Location message with a usable position */
    bool    have_operator;   /* a System message with a usable operator position */
    bool    have_id;         /* a Basic ID message with a serial */

    int32_t lat_e7, lon_e7;          /* the aircraft */
    int32_t op_lat_e7, op_lon_e7;    /* whoever is flying it */

    /* Altitude above the WGS-84 ellipsoid and height above the take-off
     * point, in metres. The wire carries half-metre steps; these are rounded,
     * because half a metre is noise for anything this is used for. */
    int16_t alt_geo_m;
    int16_t height_m;
    bool    have_alt;
    bool    have_height;

    /* Exact: the wire's quarter- and half-metre-per-second steps are whole
     * numbers of centimetres per second. */
    uint16_t speed_cmps;
    int16_t  vspeed_cmps;
    bool     have_speed;

    uint16_t direction_deg;  /* 0-359, true north */
    bool     have_direction;

    uint8_t  status;         /* operational status, as transmitted */
    char     uas_id[21];     /* serial or registration, NUL-terminated */
} observore_odid_t;

/* Message types, for the record and for tests. */
#define OBSERVORE_ODID_MSG_BASIC_ID 0x0
#define OBSERVORE_ODID_MSG_LOCATION 0x1
#define OBSERVORE_ODID_MSG_SYSTEM   0x4
#define OBSERVORE_ODID_MSG_PACKED   0xF

/* One message is always this long, and a pack may hold at most this many. A
 * pack that claims otherwise is refused rather than walked. */
#define OBSERVORE_ODID_MSG_LEN      25
#define OBSERVORE_ODID_PACK_MAX      9

/* Decode an ASTM Remote ID payload.
 *
 * `sd` begins with the ASTM application code (0x0D), then a message counter,
 * then one 25-byte message -- or a message pack holding several.
 *
 * The same function serves both transports, which is not a convenience but a
 * property of the format: over BLE this is the service data under UUID
 * 0xFFFA, and over Wi-Fi it is a vendor-specific element whose body is the
 * ASTM OUI followed by a vendor type of 0x0D -- the same byte, in the same
 * position relative to what follows it. The Wi-Fi caller passes the body
 * three bytes in, past the OUI, and the rest is identical.
 *
 * Fills whichever fields the broadcast actually carried and leaves the rest
 * alone; a drone sends its position, its operator and its serial in separate
 * messages, so one advert rarely has everything. Returns true when anything
 * usable was found.
 *
 * `out` is zeroed first, so a caller that accumulates across adverts should
 * merge rather than reuse it.
 */
bool observore_odid_parse(const uint8_t *sd, size_t len,
                          observore_odid_t *out);

/* Merge anything newly decoded into an accumulating record, keeping fields
 * already known. A drone's position arrives in one message and its operator
 * in another, often adverts apart. */
void observore_odid_merge(observore_odid_t *into, const observore_odid_t *from);

/* "51.5074,-0.1278" into `buf`, from the e7 integers, without floating point.
 * Returns the number of characters written. */
size_t observore_odid_format_pos(int32_t lat_e7, int32_t lon_e7,
                                 char *buf, size_t len);
