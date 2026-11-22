# ADR-0011: First-time setup over an open access point

- Status: accepted
- Date: 2026-11-22

## Context

A new gateway needs Wi-Fi credentials and an admin password before the web
UI (ADR-0010) is useful. Until now both needed the USB console. The ESP32-C6
runs the radio; ESP-Hosted 3.1 supports SoftAP and AP+STA, with AP traffic
carried to the P4 (`esp_wifi_remote`).

## Decision

- With no Wi-Fi credentials in NVS, the gateway runs AP+STA: it scans once
  (the names fill the setup form), then opens an **open** access point
  `lorawan-gateway-XXXXXX` (last 3 MAC bytes) on channel 6, at most 2
  clients, DHCP server on 192.168.4.1. It closes after **15 minutes**; a
  power cycle opens it again.
- The web UI shows a setup form only while the access point is open
  (`GET /api/setup` → 404 otherwise):
  - no admin password yet (**claim**): the form sets the Wi-Fi credentials
    and the first admin password together;
  - admin password already set (Wi-Fi cleared later): the form needs an
    admin session.
- On save the gateway stores both, replies, and restarts onto the network.
- Same protections as the rest of the UI: HTTPS with the device certificate,
  LAN-only peer check (192.168.4.x is private), CSRF header, lengths checked
  (SSID 1–32, Wi-Fi password 8–63, admin 8–63), secrets wiped after use.

## Consequences

- Anyone in radio range during the 15-minute window of an unconfigured unit
  can claim it. Accepted for setup; once claimed, the access point never
  opens again unless the Wi-Fi credentials are cleared, and then it needs
  the admin login.
- No captive-portal DNS: the user opens `https://192.168.4.1/` by hand and
  accepts the certificate (fingerprint on the console).
- Open Wi-Fi networks (no password) are not supported, as with `WIFI_SET`.

## Verification (2026-11-22, bench board)

| Test | Result |
| --- | --- |
| Provisioned boot | no access point; joins Wi-Fi as before |
| Credentials cleared (`WIFI_CLEAR`) | AP `lorawan-gateway-…` up, DHCP server on 192.168.4.1, 10 networks scanned, HTTPS up |
| `GET`/`POST /api/setup` on a provisioned gateway | 404 |
| Setup form | rendered at phone width (mock data) |
| Joining the AP and saving the form | **not yet tested**: needs a phone near the board (the build Mac's internet is on Wi-Fi) |
