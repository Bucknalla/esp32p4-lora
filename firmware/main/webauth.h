/*
 * Web UI identity and passwords (ADR-0010).
 *   - TLS: an ECDSA P-256 key and self-signed certificate, made on the
 *     device at first start and kept in NVS ("web").
 *   - Passwords for the admin and viewer roles: PBKDF2-HMAC-SHA256 with a
 *     random salt, kept in NVS. Set over the USB console (WEB_PASS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lwc/session.h"

#define WEBAUTH_HOST_LEN 24

/** "lorawan-gateway-xxxxxx" (last 3 MAC bytes): mDNS name and certificate CN/SAN. */
void webauth_hostname(char out[WEBAUTH_HOST_LEN]);

/** Load or create the key and certificate (DER). Pointers stay valid. */
esp_err_t webauth_cert(const uint8_t **crt, size_t *crt_len, const uint8_t **key, size_t *key_len);

/** SHA-256 of the certificate as "AA:BB:..." (95 chars + NUL), for pinning. */
void webauth_fingerprint(char *out, size_t cap);

/** Set a role's password (8..63 bytes). Ends nothing by itself. */
esp_err_t webauth_set_password(lwc_role_t role, const char *pass, size_t len);

/** True when the role has a password. */
bool webauth_has_password(lwc_role_t role);

/** Check a password for a role (slow on purpose: PBKDF2). */
bool webauth_verify(lwc_role_t role, const char *pass, size_t len);
