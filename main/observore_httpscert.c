#include "observore_httpscert.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

/* ------------------------------------------------------------------ */
/* Formatting -- pure, and the part the owner actually reads           */
/* ------------------------------------------------------------------ */

bool observore_fp_format(const uint8_t *fp, char *out, size_t out_len)
{
    if (!fp || !out || out_len < OBSERVORE_FP_HEX_LEN) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < OBSERVORE_FP_BYTES; i++) {
        if (i) {
            out[o++] = ':';
        }
        out[o++] = HEX[fp[i] >> 4];
        out[o++] = HEX[fp[i] & 0x0f];
    }
    out[o] = '\0';
    return true;
}

bool observore_fp_short(const uint8_t *fp, char *out, size_t out_len)
{
    if (!fp || !out || out_len < OBSERVORE_FP_SHORT_LEN) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < OBSERVORE_FP_SHORT_BYTES; i++) {
        /* A space every two bytes: four hex characters to a group, which is
         * what the eye can hold while it looks from the glass to the screen
         * and back. */
        if (i && (i % 2) == 0) {
            out[o++] = ' ';
        }
        out[o++] = HEX[fp[i] >> 4];
        out[o++] = HEX[fp[i] & 0x0f];
    }
    out[o] = '\0';
    return true;
}

#ifndef OBSERVORE_HOST_TEST

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_CONSOLE_HTTPS

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"

#include "observore_heapwatch.h"
#include "observore_nvs.h"

static const char *TAG = "observore.httpscert";

/* A P-256 certificate and its key, PEM, with room to spare. Measured at 635
 * and 227 bytes by running this same construction on the host against the
 * mbedTLS that IDF ships; the slack is for a longer common name, not for a
 * different key type -- an RSA key here would not fit and is not wanted (see
 * the header). */
#define CERT_PEM_MAX 1024
#define KEY_PEM_MAX  512

/* Serving a handshake costs more than making one. The outbound guard in
 * observore_update.c refuses below 20 KB; a server also holds a session for
 * the life of the connection, so this asks for more.
 *
 * Be clear that this number is CHOSEN, not measured. Nothing serves TLS yet,
 * so there is no serving measurement to derive it from: 20 KB is the figure
 * the outbound path earned, and this is that plus headroom. It wants replacing
 * with a measured low-water mark once the server exists. Calling a chosen
 * number measured is how 15,360 bytes got quoted against the wrong heap pool.
 *
 * It is a refusal, not a clamp. A console that answers over HTTP is worth more
 * than one that was supposed to be encrypted and is not there at all. */
#define SERVE_MIN_HEAP 32768

static char    s_cert[CERT_PEM_MAX];
static char    s_key[KEY_PEM_MAX];
static uint8_t s_fp[OBSERVORE_FP_BYTES];
static bool    s_have;

/* ------------------------------------------------------------------ */
/* Generation                                                         */
/* ------------------------------------------------------------------ */

static int seed(mbedtls_ctr_drbg_context *drbg, mbedtls_entropy_context *ent)
{
    mbedtls_entropy_init(ent);
    mbedtls_ctr_drbg_init(drbg);
    /* esp_fill_random is backed by the hardware RNG, which on these parts is
     * only properly random once the radio is up -- but mbedtls_entropy already
     * has the IDF entropy source registered, so this is belt and braces rather
     * than the only source. The personalisation string keeps two boards that
     * boot identically from producing the same key. */
    uint8_t pers[16];
    esp_fill_random(pers, sizeof(pers));
    return mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, ent,
                                 pers, sizeof(pers));
}

static esp_err_t generate(void)
{
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       pk;
    mbedtls_x509write_cert   crt;
    int                      rc;
    esp_err_t                err = ESP_FAIL;

    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);

    if ((rc = seed(&drbg, &ent)) != 0) {
        ESP_LOGE(TAG, "could not seed the RNG: -0x%04x", -rc);
        goto done;
    }

    /* One scalar multiplication, not a prime search. */
    if ((rc = mbedtls_pk_setup(&pk,
             mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
        (rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                  mbedtls_pk_ec(pk),
                                  mbedtls_ctr_drbg_random, &drbg)) != 0) {
        ESP_LOGE(TAG, "could not generate a key: -0x%04x", -rc);
        goto done;
    }

    /* The name is the device's own hostname. It will not match the IP a
     * browser is pointed at, and that is not what the name is for here: the
     * fingerprint is what gets verified. Putting something honest in the
     * subject is still better than putting something that looks like a real
     * authority. */
    char name[96];
    uint8_t mac[6] = {0};
    esp_err_t me = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (me != ESP_OK) {
        ESP_LOGW(TAG, "could not read the MAC: %s", esp_err_to_name(me));
    }
    snprintf(name, sizeof(name), "CN=observore-%02x%02x%02x,O=Observore",
             mac[3], mac[4], mac[5]);

    /* A random serial, because a fixed one would make every board's
     * certificate collide in any store that indexes by issuer and serial.
     *
     * set_serial_raw, not set_serial: mbedTLS 3.x removed the MPI form. The
     * top bit is cleared because a DER INTEGER with it set is negative, and
     * the leading byte is forced non-zero because a leading zero byte is not
     * a valid minimal encoding. */
    uint8_t ser[16];
    esp_fill_random(ser, sizeof(ser));
    ser[0] &= 0x7f;
    if (ser[0] == 0) {
        ser[0] = 1;
    }
    if ((rc = mbedtls_x509write_crt_set_serial_raw(&crt, ser,
                                                   sizeof(ser))) != 0) {
        ESP_LOGE(TAG, "could not set the serial: -0x%04x", -rc);
        goto done;
    }

    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);   /* self-signed */
    if ((rc = mbedtls_x509write_crt_set_subject_name(&crt, name)) != 0 ||
        (rc = mbedtls_x509write_crt_set_issuer_name(&crt, name)) != 0) {
        ESP_LOGE(TAG, "could not set the certificate names: -0x%04x", -rc);
        goto done;
    }
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    /* Validity is stated rather than enforced.
     *
     * This device frequently has no idea what time it is -- that is a whole
     * section of its own documentation -- so a certificate it generates at
     * first boot cannot have an honest notBefore. A browser that checks the
     * dates will complain, and it was going to complain anyway about a
     * self-signed certificate: the fingerprint is the thing being verified,
     * and expiry adds nothing to that while a short window would mean the
     * fingerprint the owner wrote down stops working.
     *
     * So: a wide window, from before these parts existed to long after this
     * code is someone else's problem. */
    if ((rc = mbedtls_x509write_crt_set_validity(&crt, "20200101000000",
                                                 "20400101000000")) != 0) {
        goto done;
    }
    /* Not a CA, and say so, so nothing can be persuaded to sign with it. */
    if ((rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, 0)) != 0 ||
        (rc = mbedtls_x509write_crt_set_subject_key_identifier(&crt)) != 0 ||
        (rc = mbedtls_x509write_crt_set_authority_key_identifier(&crt)) != 0) {
        ESP_LOGE(TAG, "could not set the extensions: -0x%04x", -rc);
        goto done;
    }

    unsigned char pem[CERT_PEM_MAX];
    if ((rc = mbedtls_x509write_crt_pem(&crt, pem, sizeof(pem),
                                        mbedtls_ctr_drbg_random, &drbg)) != 0) {
        ESP_LOGE(TAG, "could not write the certificate: -0x%04x", -rc);
        goto done;
    }
    if (strlen((char *)pem) >= sizeof(s_cert)) {
        ESP_LOGE(TAG, "certificate is %u bytes, buffer is %u",
                 (unsigned)strlen((char *)pem), (unsigned)sizeof(s_cert));
        goto done;
    }
    strcpy(s_cert, (char *)pem);

    unsigned char kpem[KEY_PEM_MAX];
    if ((rc = mbedtls_pk_write_key_pem(&pk, kpem, sizeof(kpem))) != 0) {
        ESP_LOGE(TAG, "could not write the key: -0x%04x", -rc);
        goto done;
    }
    if (strlen((char *)kpem) >= sizeof(s_key)) {
        goto done;
    }
    strcpy(s_key, (char *)kpem);

    err = ESP_OK;
done:
    /* The key was in both of these. */
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return err;
}

/* ------------------------------------------------------------------ */
/* The fingerprint, taken from the DER the way a browser takes it      */
/* ------------------------------------------------------------------ */

static esp_err_t fingerprint(const char *cert_pem, uint8_t *out)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    /* The length must include the terminator for PEM input. */
    int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem,
                                    strlen(cert_pem) + 1);
    if (rc != 0) {
        ESP_LOGE(TAG, "stored certificate does not parse: -0x%04x", -rc);
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }
    /* crt.raw is the DER, which is what the browser hashes. Hashing the PEM
     * would give a stable number that matches nothing the owner can see. */
    rc = mbedtls_sha256(crt.raw.p, crt.raw.len, out, 0);
    mbedtls_x509_crt_free(&crt);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Storage                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t load(void)
{
    observore_nvs_item_t items[] = {
        {.key = "tlscert", .type = OBSERVORE_NVS_STR,
         .buf = s_cert, .len = sizeof(s_cert)},
        {.key = "tlskey",  .type = OBSERVORE_NVS_STR,
         .buf = s_key,  .len = sizeof(s_key)},
    };
    observore_nvs_read(items, 2);
    if (!items[0].found || !items[1].found ||
        s_cert[0] == '\0' || s_key[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t store(void)
{
    const observore_nvs_item_t items[] = {
        {.key = "tlscert", .type = OBSERVORE_NVS_STR,
         .buf = s_cert, .len = strlen(s_cert) + 1},
        {.key = "tlskey",  .type = OBSERVORE_NVS_STR,
         .buf = s_key,  .len = strlen(s_key) + 1},
    };
    /* One commit, so a power cut cannot leave a certificate with somebody
     * else's key beside it -- which would serve a handshake that fails in a
     * way nothing here would explain. */
    return observore_nvs_write(items, 2);
}

void observore_httpscert_init(void)
{
    if (s_have) {
        return;
    }

    esp_err_t err = load();
    if (err == ESP_ERR_NOT_FOUND) {
        /* First boot with HTTPS built in. Generating now, before the radio
         * starts, is deliberate: this is the point in the device's life with
         * the most free internal heap, and the cost is paid once. */
        ESP_LOGI(TAG, "no certificate stored; generating one");
        uint32_t before = heap_caps_get_free_size(OBSERVORE_HEAP_CAPS);
        if (generate() != ESP_OK) {
            memset(s_cert, 0, sizeof(s_cert));
            memset(s_key, 0, sizeof(s_key));
            return;
        }
        uint32_t after = heap_caps_get_free_size(OBSERVORE_HEAP_CAPS);
        ESP_LOGI(TAG, "generated a P-256 certificate (%u bytes) and its key; "
                 "internal heap %" PRIu32 " -> %" PRIu32,
                 (unsigned)strlen(s_cert), before, after);
        if (store() != ESP_OK) {
            /* Usable this run, gone at the next boot. Say so, because a
             * fingerprint the owner verified and wrote down would silently
             * stop matching. */
            ESP_LOGW(TAG, "could not store the certificate; it will be "
                          "regenerated at the next boot and the fingerprint "
                          "will change");
        }
    } else if (err != ESP_OK) {
        return;
    }

    if (fingerprint(s_cert, s_fp) != ESP_OK) {
        memset(s_cert, 0, sizeof(s_cert));
        memset(s_key, 0, sizeof(s_key));
        memset(s_fp, 0, sizeof(s_fp));
        return;
    }

    s_have = true;

    /* On serial, in full, once. The same cable the console password needs.
     * This is not a secret -- a certificate fingerprint is public by
     * construction -- so unlike the password it is also safe to serve. */
    char hex[OBSERVORE_FP_HEX_LEN];
    if (observore_fp_format(s_fp, hex, sizeof(hex))) {
        ESP_LOGW(TAG, "console certificate SHA-256 fingerprint: %s", hex);
        ESP_LOGW(TAG, "compare this with what the browser shows, once");
    }
}

bool observore_httpscert_present(void)
{
    return s_have;
}

bool observore_httpscert_get(const char **cert, size_t *cert_len,
                             const char **key, size_t *key_len)
{
    if (!s_have) {
        return false;
    }
    /* esp_https_server wants the length including the terminator. */
    *cert     = s_cert;
    *cert_len = strlen(s_cert) + 1;
    *key      = s_key;
    *key_len  = strlen(s_key) + 1;
    return true;
}

bool observore_httpscert_fp(uint8_t *out)
{
    if (!s_have || !out) {
        return false;
    }
    memcpy(out, s_fp, sizeof(s_fp));
    return true;
}

bool observore_httpscert_can_serve(uint32_t *free_heap)
{
    uint32_t free_now = heap_caps_get_free_size(OBSERVORE_HEAP_CAPS);
    if (free_heap) {
        *free_heap = free_now;
    }
    return s_have && free_now >= SERVE_MIN_HEAP;
}

#endif /* CONFIG_OBSERVORE_CONSOLE_HTTPS */
#endif /* OBSERVORE_HOST_TEST */
