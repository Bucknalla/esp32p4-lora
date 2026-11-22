# ADR-0016: LoRa Basics Station as the TTN link

- Status: proposed (next after the phase 3 PR)
- Date: 2026-11-22

## Context

The gateway reaches TTN over the Semtech UDP protocol (ADR-0012). UDP has no
encryption and no authentication: anyone who knows the gateway EUI can send
traffic as this gateway, downlinks can be lost without notice, and the TTN
console can only guess whether the gateway is connected.

With the local network server (ADR-0015), the planned main mode is hybrid:
our own devices are served on the board, and every other frame goes to TTN so
the gateway keeps serving the community network. The user asked for TTN to
see a fully featured gateway rather than a packet forwarder.

TTN recommends LoRa Basics Station's LNS protocol (Semtech, protocol 2) for
gateways.

## Decision

- **Add a Basics Station LNS client** as a second way to reach the upstream
  network. It takes the same queue the UDP forwarder takes today: everything
  in TTN mode, and in hybrid mode every frame the local server did not handle.
  Only one of the two runs at a time.
- **Setting**: Config tab, TTN card, admin only: "Connection: Basics Station
  (recommended) | Semtech UDP". UDP stays for ChirpStack and other servers
  that use it. Basics Station becomes the default for TTN once it has passed
  its on-board test.
- **Connection** (TTN documentation, read 2026-11-22):
  - LNS address `wss://<cluster>.cloud.thethings.network:8887` (cluster chosen
    on the Config tab, `eu1` by default).
  - Authentication: TTN does not support TLS client certificates. The gateway
    sends `Authorization: Bearer <API key>`. The key needs only the right
    "Link as Gateway to a Gateway Server for traffic exchange".
  - Server certificate checked with ESP-IDF's CA bundle; no pinned root.
  - Sequence (Semtech protocol): router-info request, then the traffic
    WebSocket; `version` from the gateway, `router_config` from the server.
- **API key**: entered on the Config tab, stored in NVS (encryption off,
  ADR-0002), write-only in the API, never logged, never shown again. A
  leaked key can only connect as this gateway.
- **Messages**:
  - Up: `jreq` (join-requests), `updf` (data frames), `propdf` (proprietary),
    `dntxed` (a downlink was sent).
  - Down: `dnmsg` (class A first: RX1 with RX2 as the fallback, the same
    mechanism as the local server's, ADR-0015); `router_config`.
  - Not used: class B (`dnsched`, beacons; no GPS), `timesync`, `runcmd`,
    `rmtsh` (remote commands are refused).
- **Channel plan**: `router_config` is checked against the concentrator's
  EU868 plan. A mismatch is logged and shown on the status page; the gateway
  does not reconfigure the radio from the server at first.
- **Duty cycle and power**: in Basics Station the gateway enforces the duty
  cycle (Semtech protocol doc). Our radio task already does: the JIT queue,
  the per-sub-band budget and the 13 dBm cap (ADR-0007) apply to TTN
  downlinks and local downlinks alike.
- **Timestamps**: Basics Station times downlinks with `xtime` (a session id
  and a 48-bit microsecond counter); the concentrator counter is 32 bits.
  The client extends it and refuses a downlink whose session does not match.
- **CUPS** (remote configuration and updates) is out of scope: firmware
  updates already come through our own signed OTA (ADR-0008), and CUPS would
  need a second key with more rights.

## Consequences

- Traffic to TTN is encrypted and authenticated; downlinks are confirmed
  (`dntxed`); the TTN console shows the gateway as connected while the
  WebSocket is open.
- TLS costs RAM (one connection, about 40 KB) and needs a set clock to check
  certificates; with no clock or no network, the local server keeps working.
- Local devices still do not appear in TTN: TTN sees the gateway only.
  Linking the local server into TTN's network (Packet Broker) needs an
  agreement with The Things Industries and is not planned.

## Open, to check while building

- Whether TTN's endpoint uses the router-info step or a direct traffic path
  (TTN's documentation gives only the address and port).
- That ESP-IDF's CA bundle includes the root TTN's certificate chains to.

## Plan

1. Core: Basics Station message codec (`version`, `jreq`, `updf`, `dnmsg`,
   `router_config`, `dntxed`), `xtime` handling; host tests and a fuzz
   target, like the UDP code.
2. Firmware: TLS WebSocket client, reconnect with backoff, the API key
   setting, Config tab and status page; on-board test against TTN.
