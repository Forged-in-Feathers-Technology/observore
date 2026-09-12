#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Console authentication.
 *
 * Scope is uplink mode. The SoftAP is already gated by WPA2 with the same
 * per-device password, so asking for it a second time there would add friction
 * to first-time setup -- which is the one moment the console is the only way
 * in -- for no gain.
 *
 * The credential is the generated console password, deliberately not a second
 * secret to lose. Because that password is ALSO the SoftAP's WPA2 key, it is
 * exchanged once at login and everything after that carries a session token
 * instead: a listener on the LAN then learns a token that expires, rather than
 * the key to the device's own access point.
 *
 * Over plain HTTP a token can still be captured. This stops casual and
 * accidental access; it does not stop a listener. That is issue #11. */

void observore_auth_init(void);

/* Whether this request may proceed. True when authentication is disabled, when
 * the SoftAP link already provides it, or when a valid session is presented. */
bool observore_auth_ok(httpd_req_t *req);

/* Exchange the console password for a session. Writes a Set-Cookie header on
 * success. Returns ESP_ERR_INVALID_ARG for a wrong password. */
esp_err_t observore_auth_login(httpd_req_t *req, const char *password);

/* Drop the session this request carries. */
void observore_auth_logout(httpd_req_t *req);

/* Whether a client would be challenged right now -- lets the console show a
 * login form without guessing. */
bool observore_auth_enforced(void);
