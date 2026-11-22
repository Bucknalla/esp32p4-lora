# ADR-0012: TTN reporting switch and registration help (no TTN QR claim)

- Status: partly superseded by ADR-0015 (the on/off switch became the gateway mode; the registration card stays). Settings live on the Config tab; TTN is not a setup step (ADR-0014)
- Date: 2026-11-22

## Context

The user wants a QR code to register the gateway on The Things Network when
the gateway reports there. The Things Stack's gateway QR parser knows one
format, `TTIGPRO1` (`https://ttig.pro/c/<EUI>/<owner token>`,
`pkg/qrcodegenerator/qrcode/gateways/ttigpro1.go`), and scanning it starts a
**claim** that The Things Industries can only complete for its own Indoor
Gateway Pro units. A DIY gateway must be registered by hand with its EUI.

## Decision

- A setting **Report to The Things Network (eu1)**, stored in NVS
  (`pktfwd/ttn`, default on as before). Set on the setup page (ADR-0011) and on
  the web UI's Config tab (admin, `POST /api/settings ttn=on|off`); it applies
  after a restart. Off: the packet forwarder does not start.
- While it is on, the Config tab shows a **Register this gateway**
  card: the gateway EUI with a copy button, a QR code of the EUI (to scan with
  a phone into the console's EUI field), the frequency plan and server
  address to choose, and a link to the eu1 console's Register gateway page.
- The QR code is made on the device as SVG (`espressif/qrcode`, Apache-2.0)
  at `GET /api/ttn/qr.svg` (viewer): the UI's CSP allows nothing from outside
  the board.
- No TTN credentials touch the gateway.

## Consequences

- Registration stays a manual step in the TTN console; the card removes the
  typing and the guesswork.
- Other servers (TTN clusters, ChirpStack) are still a build-time Kconfig
  value; a full server setting can follow.

## Verification (2026-11-22, bench board)

| Test | Result |
| --- | --- |
| QR code | 3.3 KB SVG; decoded with macOS CoreImage → the gateway EUI the forwarder uses |
| QR / settings access | no session 401; viewer `POST /api/settings` 403 |
| `ttn=off` then `ttn=on` | 202 and restart each time; off: forwarder not started, upstream down, card hidden; on: TTN link up again |
| Console link | `https://eu1.cloud.thethings.network/console/gateways/add`, not checked while logged in |
