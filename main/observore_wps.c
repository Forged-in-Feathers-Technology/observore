#include "observore_wps.h"

#include <string.h>

/* Wi-Fi Simple Configuration lives in a vendor-specific element (id 221) whose
 * OUI is 00:50:F2 with vendor type 0x04. */
#define IE_VENDOR_SPECIFIC 0xDD
static const uint8_t WPS_OUI[3] = {0x00, 0x50, 0xF2};
#define WPS_OUI_TYPE 0x04

/* Attribute identifiers, from the Wi-Fi Simple Configuration specification.
 * Taken from wpa_supplicant's wps_defs.h rather than from memory. */
#define ATTR_DEV_NAME     0x1011
#define ATTR_MANUFACTURER 0x1021
#define ATTR_MODEL_NAME   0x1023

/* Copy a WPS string somewhere it is safe to print.
 *
 * The source is a radio frame from a device that has not been asked to be
 * honest, so this keeps only printable ASCII and always terminates. Anything
 * else -- control characters, a stray quote heading for a JSON document, a
 * length that runs off the end of the frame -- is dropped here rather than
 * being someone else's problem later. */
static void copy_printable(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_len; i++) {
        unsigned char c = src[i];
        if (c >= 0x20 && c < 0x7F) {
            dst[out++] = (char)c;
        }
    }
    /* Trailing blanks are common in these fields and read as ragged. */
    while (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
}

/* Walk the attributes inside one WPS element. Type and length are both
 * 16-bit big-endian here, which is not the 8-bit convention the surrounding
 * 802.11 elements use -- mixing the two up reads garbage. */
static void parse_attributes(const uint8_t *p, size_t len, observore_wps_t *out)
{
    size_t i = 0;
    while (i + 4 <= len) {
        uint16_t type = (uint16_t)((p[i] << 8) | p[i + 1]);
        uint16_t alen = (uint16_t)((p[i + 2] << 8) | p[i + 3]);
        i += 4;
        if (alen > len - i) {
            return;                    /* runs past the element: stop */
        }
        switch (type) {
            case ATTR_MANUFACTURER:
                copy_printable(out->manufacturer, sizeof(out->manufacturer), p + i, alen);
                break;
            case ATTR_MODEL_NAME:
                copy_printable(out->model, sizeof(out->model), p + i, alen);
                break;
            case ATTR_DEV_NAME:
                copy_printable(out->device_name, sizeof(out->device_name), p + i, alen);
                break;
            default:
                break;
        }
        i += alen;
    }
}

bool observore_wps_from_ies(const uint8_t *ies, size_t len, observore_wps_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!ies) {
        return false;
    }

    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i];
        uint8_t ie_len = ies[i + 1];
        if (ie_len > len - i - 2) {
            break;                     /* truncated frame */
        }
        const uint8_t *body = &ies[i + 2];
        if (id == IE_VENDOR_SPECIFIC && ie_len >= 4 &&
            memcmp(body, WPS_OUI, sizeof(WPS_OUI)) == 0 &&
            body[3] == WPS_OUI_TYPE) {
            parse_attributes(body + 4, (size_t)ie_len - 4, out);
        }
        i += 2 + ie_len;
    }
    return !observore_wps_empty(out);
}

bool observore_wps_empty(const observore_wps_t *w)
{
    return !w || (!w->manufacturer[0] && !w->model[0] && !w->device_name[0]);
}
