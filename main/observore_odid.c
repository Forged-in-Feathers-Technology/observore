#include "observore_odid.h"

#include <stdio.h>
#include <string.h>

/* The ASTM application code that opens the service data. Anything else under
 * UUID 0xFFFA is a different ASTM application and not ours to read. */
#define ASTM_APP_CODE_ODID 0x0D

/* Degrees times ten million, as the wire carries them. */
#define LATLON_MULT 10000000

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* A position the device will repeat has to be one it believes.
 *
 * Zero latitude and zero longitude is the Gulf of Guinea, and it is what a
 * drone transmits before it has a GPS fix -- the reference implementation
 * treats it as unset too. Reporting it would be a confident lie about where
 * something is, which is worse than saying nothing, so it is refused along
 * with anything outside the possible range. */
static bool position_usable(int32_t lat_e7, int32_t lon_e7)
{
    if (lat_e7 == 0 && lon_e7 == 0) {
        return false;
    }
    if (lat_e7 > 90 * LATLON_MULT || lat_e7 < -90 * LATLON_MULT) {
        return false;
    }
    if (lon_e7 > 180 * LATLON_MULT || lon_e7 < -180 * LATLON_MULT) {
        return false;
    }
    return true;
}

/* Half-metre steps with a thousand-metre bias, rounded to the metre.
 *
 * The bias is why this cannot be a plain multiply: the wire's zero means a
 * kilometre below the ellipsoid, not sea level. */
static int16_t decode_alt_m(uint16_t enc)
{
    int32_t halves = (int32_t)enc - 2000;      /* 1000 m of bias, in halves */
    int32_t m = (halves >= 0) ? (halves + 1) / 2 : (halves - 1) / 2;
    if (m > INT16_MAX) { m = INT16_MAX; }
    if (m < INT16_MIN) { m = INT16_MIN; }
    return (int16_t)m;
}

static void decode_location(const uint8_t *m, observore_odid_t *out)
{
    /* Byte 1 carries four flags in its low nibble and the status in its high
     * one. The two that change how later bytes are read are the speed
     * multiplier and the east/west direction segment. */
    bool speed_mult = (m[1] & 0x01) != 0;
    bool ew         = (m[1] & 0x02) != 0;
    out->status     = (uint8_t)((m[1] >> 4) & 0x0F);

    /* Direction is 0-179 with a flag that adds the other half of the compass,
     * which is how a whole circle fits in one byte. 361 means unknown. */
    if (m[2] <= 179) {
        out->direction_deg = (uint16_t)(m[2] + (ew ? 180 : 0));
        out->have_direction = true;
    }

    /* Quarter-metre steps, or three-quarter steps above 63.75 m/s. Both are
     * whole centimetres per second, so no floating point is needed. */
    if (m[3] != 255) {
        out->speed_cmps = speed_mult
            ? (uint16_t)((uint16_t)m[3] * 75u + 6375u)
            : (uint16_t)((uint16_t)m[3] * 25u);
        out->vspeed_cmps = (int16_t)((int16_t)(int8_t)m[4] * 50);
        out->have_speed = true;
    }

    int32_t lat = le32(&m[5]);
    int32_t lon = le32(&m[9]);
    if (position_usable(lat, lon)) {
        out->lat_e7 = lat;
        out->lon_e7 = lon;
        out->have_location = true;
    }

    /* Geodetic altitude rather than barometric: the barometric figure depends
     * on a reference pressure nobody here knows. */
    uint16_t alt_geo = le16(&m[15]);
    if (alt_geo != 0) {
        out->alt_geo_m = decode_alt_m(alt_geo);
        out->have_alt = true;
    }
    uint16_t height = le16(&m[17]);
    if (height != 0) {
        out->height_m = decode_alt_m(height);
        out->have_height = true;
    }
}

static void decode_system(const uint8_t *m, observore_odid_t *out)
{
    int32_t lat = le32(&m[2]);
    int32_t lon = le32(&m[6]);
    if (position_usable(lat, lon)) {
        out->op_lat_e7 = lat;
        out->op_lon_e7 = lon;
        out->have_operator = true;
    }
}

static void decode_basic_id(const uint8_t *m, observore_odid_t *out)
{
    /* Twenty bytes of ASCII, not guaranteed to be terminated. Anything
     * unprintable ends it: a serial with control characters in it is either
     * padding or something trying to be interesting in a log. */
    char id[21];
    size_t n = 0;
    for (; n < 20; n++) {
        char c = (char)m[2 + n];
        if (c < 0x20 || c > 0x7E) {
            break;
        }
        id[n] = c;
    }
    id[n] = '\0';
    if (n > 0) {
        memcpy(out->uas_id, id, n + 1);
        out->have_id = true;
    }
}

static bool decode_message(const uint8_t *m, observore_odid_t *out)
{
    uint8_t type = (uint8_t)((m[0] >> 4) & 0x0F);
    switch (type) {
    case OBSERVORE_ODID_MSG_LOCATION: decode_location(m, out); return true;
    case OBSERVORE_ODID_MSG_SYSTEM:   decode_system(m, out);   return true;
    case OBSERVORE_ODID_MSG_BASIC_ID: decode_basic_id(m, out); return true;
    default:
        /* Authentication, self-ID and operator-ID carry nothing this needs
         * yet. Recognised and skipped rather than treated as malformed. */
        return false;
    }
}

bool observore_odid_parse_ble(const uint8_t *sd, size_t len,
                              observore_odid_t *out)
{
    if (!sd || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* Application code, message counter, then the message itself. */
    if (len < 2 + OBSERVORE_ODID_MSG_LEN || sd[0] != ASTM_APP_CODE_ODID) {
        return false;
    }
    const uint8_t *m = sd + 2;
    size_t avail = len - 2;

    uint8_t type = (uint8_t)((m[0] >> 4) & 0x0F);
    if (type != OBSERVORE_ODID_MSG_PACKED) {
        return decode_message(m, out);
    }

    /* A pack declares its own geometry, and a broadcast is not a trustworthy
     * narrator: both the stride and the count are checked against what the
     * format allows and against what actually arrived. Walking a pack that
     * claims two hundred messages is how a malformed advert reads past the
     * end of the buffer it came in. */
    uint8_t stride = m[1];
    uint8_t count  = m[2];
    if (stride != OBSERVORE_ODID_MSG_LEN || count == 0 ||
        count > OBSERVORE_ODID_PACK_MAX) {
        return false;
    }
    if (avail < 3u + (size_t)count * OBSERVORE_ODID_MSG_LEN) {
        return false;
    }

    bool any = false;
    for (uint8_t i = 0; i < count; i++) {
        if (decode_message(m + 3 + (size_t)i * OBSERVORE_ODID_MSG_LEN, out)) {
            any = true;
        }
    }
    return any;
}

void observore_odid_merge(observore_odid_t *into, const observore_odid_t *from)
{
    if (!into || !from) {
        return;
    }
    if (from->have_location) {
        into->lat_e7 = from->lat_e7;
        into->lon_e7 = from->lon_e7;
        into->have_location = true;
        /* The status travels with the position: it describes the aircraft at
         * the moment that position was true. */
        into->status = from->status;
    }
    if (from->have_operator) {
        into->op_lat_e7 = from->op_lat_e7;
        into->op_lon_e7 = from->op_lon_e7;
        into->have_operator = true;
    }
    if (from->have_id) {
        memcpy(into->uas_id, from->uas_id, sizeof(into->uas_id));
        into->have_id = true;
    }
    if (from->have_alt)    { into->alt_geo_m = from->alt_geo_m; into->have_alt = true; }
    if (from->have_height) { into->height_m = from->height_m; into->have_height = true; }
    if (from->have_speed) {
        into->speed_cmps = from->speed_cmps;
        into->vspeed_cmps = from->vspeed_cmps;
        into->have_speed = true;
    }
    if (from->have_direction) {
        into->direction_deg = from->direction_deg;
        into->have_direction = true;
    }
}

size_t observore_odid_format_pos(int32_t lat_e7, int32_t lon_e7,
                                 char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    /* Four decimal places: about eleven metres, which is the right precision
     * for "a drone is over there" and avoids implying a survey. Printed from
     * the integers so no float is involved -- and so that the sign of a
     * fractional part near zero cannot be lost, which is what happens when
     * -0.05 degrees is split into a whole part of 0 and a remainder. */
    int32_t v[2] = {lat_e7, lon_e7};
    char part[2][16];
    for (int i = 0; i < 2; i++) {
        int32_t x = v[i];
        const char *sign = (x < 0) ? "-" : "";
        uint32_t mag = (uint32_t)((x < 0) ? -(int64_t)x : (int64_t)x);
        uint32_t whole = mag / LATLON_MULT;
        uint32_t frac  = (mag % LATLON_MULT) / 1000;   /* 1e7 -> 1e4 */
        snprintf(part[i], sizeof(part[i]), "%s%lu.%04lu", sign,
                 (unsigned long)whole, (unsigned long)frac);
    }
    int n = snprintf(buf, len, "%s,%s", part[0], part[1]);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return ((size_t)n >= len) ? len - 1 : (size_t)n;
}
