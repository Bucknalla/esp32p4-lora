# ADR-0010: Management web UI and REST API over HTTPS, LAN only

- Status: accepted
- Date: 2026-11-22

## Context

The plan replaces SSH on a Pi with a web UI and REST API: status, logs,
coredump download, firmware actions, later settings and devices. The board
is on a home LAN. Secure boot, flash encryption and NVS encryption stay off
(ADR-0002, ADR-0006), so anything stored on the device is readable from a
flash dump.

## Decision

- **Transport:** `esp_https_server` on port 443. On first start the gateway
  creates an ECDSA P-256 key and a self-signed certificate (PSA key
  generation, mbedTLS x509write; CN `lorawan-gateway-<last 3 MAC bytes>`,
  SAN `lorawan-gateway.local`, valid 2025–2099 because there is no trusted
  clock at first start) and keeps both in NVS. Its SHA-256 fingerprint is
  logged at boot and shown by console `STATUS`, so tools can pin it and a
  browser user can compare it once.
- **LAN only:** every request's peer address must be private, link-local or
  loopback (`core/src/netaddr.c`, host-tested); others get 403.
- **Login:** two roles, `admin` and `viewer`. Passwords (8–63 bytes) are
  set over the USB console (`WEB_PASS`, `tools/web_password.py`) and stored as
  PBKDF2-HMAC-SHA256 (10 000 rounds, 16-byte random salt). A login gives a
  random 128-bit session cookie (`Secure; HttpOnly; SameSite=Strict`), idle
  timeout 30 min, absolute 12 h, at most 8 sessions. A password change logs
  everyone out. After 5 failed logins, logins are refused for 60 s, doubling
  up to 15 min.
- **CSRF:** every POST/DELETE needs `X-Requested-With: gw`; a cross-site form
  cannot set it and the server answers no CORS preflight.
- **Headers:** CSP `default-src 'self'` (no inline script), `nosniff`,
  `no-referrer`, HSTS, `no-store` on API responses.
- **UI:** one HTML page, one script, one stylesheet, gzipped at build time and
  embedded in the app image (no LittleFS yet). The script only uses
  `textContent`.
- **API:** `GET /api/status`, `GET /api/logs[?prev=1]` (viewer);
  `GET|DELETE /api/coredump`, `POST /api/ota/check`, `POST /api/reboot` (admin).

## Consequences

- The browser warns about the certificate until the user accepts it once;
  scripts pin the fingerprint.
- The TLS key and the password hashes are in plain NVS: someone with the
  board can read them. The hashes are salted and slow, not secret.
- One TLS handshake plus PBKDF2 makes a login take about 1.2 s.
- Internal heap after start: ~334 KB free (was ~348 KB).
- Not done yet: settings, live frames, devices (phase 3), mDNS.

## Verification (2026-11-22, bench board, Wi-Fi, from the LAN)

| Test | Result |
| --- | --- |
| `GET /` | 200, gzip, CSP header, 695 B |
| API without a session, or with a forged cookie | 401 |
| POST without `X-Requested-With` (login and admin action) | 403 |
| Viewer: status, logs | 200 |
| Viewer: OTA check, coredump | 403 |
| Admin: coredump download | 200, 15.5 KB, valid core dump header |
| Logout | that cookie gets 401, other sessions stay |
| 5 wrong passwords | 6th try 429; the right password is refused during the lockout |
| Served logs | contain no password and no submitted user name |
| Certificate | created on device in 106 ms; fingerprint pinned by the test client |
