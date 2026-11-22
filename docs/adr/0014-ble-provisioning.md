# ADR-0014: Wi-Fi setup over Bluetooth LE, claim from the LAN

- Status: accepted
- Date: 2026-11-22
- Supersedes: ADR-0011 (setup access point), the captive portal part of ADR-0013

## Context

The setup access point (ADR-0011) and its captive portal (ADR-0013) worked,
but the user had to leave their own Wi-Fi, accept a self-signed certificate
on a temporary network, and switch back. Phones handle BLE setup better: one
app, no network change. The P4 has no radio; the on-module ESP32-C6 has a BLE
controller, and ESP-Hosted 3.1.0 carries HCI over the same SDIO link as
Wi-Fi. iOS Safari has no Web Bluetooth, so a web page cannot do it on an
iPhone; Espressif's "ESP BLE Provisioning" app (iOS, Android) can.

## Decision

- **Transport.** NimBLE on the P4 (`CONFIG_BT_CONTROLLER_DISABLED`, host
  memory in PSRAM), bound to the C6's controller with
  `esp_hosted_bt_host_stack_setup()`. The C6 image has the BLE controller on
  with HCI over the SDIO link (VHCI). `c6_update.c` now also sends a `cp_fw`
  build with the same ESP-Hosted version when its ELF SHA-256 prefix differs
  from the last one sent (NVS `c6upd/sent`).
- **Protocol.** Espressif's provisioning protocol, so the stock app works:
  protocomm over BLE, the standard service and characteristic UUIDs, endpoints
  `proto-ver`, `prov-session`, `prov-config`, `prov-scan`. The handlers are
  ours (`ble_prov.c`), not the provisioning manager's: `wifi.c` keeps the
  credentials in NVS `wifi` and owns connect and backoff.
- **Security.** Security 1 (X25519 + AES-CTR) without a proof of possession:
  there is no label or display to show one. The window is the control, as with
  the open access point before.
- **Window.** BLE setup opens for 15 min when there are no Wi-Fi credentials
  at boot, or when the HAT button is held 5 s (GPIO5, pull-up, active low;
  `lwc_hold_update` in core, host-tested). It closes 30 s after the applied
  network gives an IP, or when the time is up. The LED blinks slowly while it
  is open.
- **Status to the app.** Connected (IP, SSID, BSSID, channel), connecting, or
  failed with "wrong password" or "network not found" from the last
  disconnect reason. Open networks are refused (WPA2 or newer, as before).
- **Claim.** The app sends Wi-Fi only. With no admin password, the first
  visit to the web UI from the LAN sets it (`GET|POST /api/claim`, with the
  TTN switch) within 15 min of start or of a BLE setup that joined a network.
  This also gives Ethernet-only installs a claim path. The console `WEB_PASS`
  always works.
- **Removed.** The setup access point, captive DNS (`core/src/dns.c`, its
  test and fuzz target), the port-80 portal, the network list and
  `/api/setup`. USB setup (`tools/provision_wifi.py`, `WIFI_SET`) and the mDNS
  name stay.
- **Fail soft.** A C6 without the BLE controller (app OTA never updates
  `cp_fw`) makes BLE setup log an error; the gateway runs on and the OTA
  health check does not depend on BLE.

## Consequences

- Setup needs the Espressif app (or IDF's `esp_prov`). Our own app can use the
  open-source provisioning SDKs and add an encrypted endpoint later (for
  example claim over BLE).
- Anyone in BLE range during an open window can set the Wi-Fi; anyone on the
  LAN during the claim window can set the admin password. Both windows are
  short and need a fresh boot or the button.
- Image +~0.3 MB (1.5 MB of 4 MB). Internal heap with BLE closed: 315 KB free
  (was ~334 KB); the C6 image is 1.43 MB (slot 1.75 MB).

## Verification (2026-11-22, bench board, iPhone, ESP BLE Provisioning app)

| Test | Result |
| --- | --- |
| C6 image with BLE, same version 3.1.0, written to `cp_fw` | P4 sent it (1.43 MB, ~11 s), restarted; C6 init event reports "BLE only"; next boot "C6 update confirmed" |
| Boot with Wi-Fi configured | BLE stays closed; Wi-Fi joins as before |
| HAT button held 5 s | "button pressed", BLE setup opens, advertises `PROV_<suffix>` |
| App: connect, scan, choose network, password | scan 16 networks; network received; gateway rejoined and got an IP ~4 s later; app showed success |
| After the join | phone disconnected; BLE closed 30 s later; internal heap back to 315 KB |
| Claim API with an admin already set | `GET /api/claim` open=false; `POST` 403; old `/api/setup` 404 |
