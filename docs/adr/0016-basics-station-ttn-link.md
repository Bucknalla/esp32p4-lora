# ADR-0016: LoRa Basics Station as the only upstream link

- Status: accepted (built and tested against TTN on the board, see
  Verification); supersedes the Semtech UDP forwarder (phase 1, ADR-0012)
- Date: 2026-11-22; updated 2026-11-23

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

- **Basics Station is the only upstream link.** The Semtech UDP forwarder
  (`pktfwd.c`, core `gwmp`, the `txpk` JSON parser, their tests and fuzz
  targets) is removed. The client takes the queue the forwarder took:
  everything in TTN mode, and in hybrid mode every frame the local server did
  not handle; in standalone mode nothing connects. UDP had no
  authentication (a known gateway EUI was enough to send as this gateway),
  and the servers in view speak Basics Station: TTN, ChirpStack v4 (through
  its gateway bridge) and AWS IoT Core for LoRaWAN.
- **Settings**: Setup tab, admin only: the server (TTN presets eu1, nam1,
  au1 on port 8887, or another host and port; `lns_host`, `lns_port`) and the
  API key. The TTN registration help shows only for a TTN host. The mode alone decides whether the gateway connects. Without a
  key the gateway does not connect, and the status page says so; hybrid
  mode keeps serving local devices. NVS: `pktfwd/lbs_host`, `pktfwd/lbs_key`
  (the namespace keeps its old name so stored settings survive).
- **Connection** (TTN documentation, read 2026-11-22):
  - LNS address `wss://<cluster>.cloud.thethings.network:8887` (cluster chosen
    on the Config tab, `eu1` by default).
  - Authentication: TTN does not support TLS client certificates. The gateway
    sends `Authorization: Bearer <API key>`. The key needs only the right
    "Link as Gateway to a Gateway Server for traffic exchange".
  - Server certificate checked with ESP-IDF's CA bundle; no pinned root.
    The chain (checked 2026-11-23) is Let's Encrypt YR1, then Root YR,
    cross-signed by ISRG Root X1, which is in the bundle. If Let's Encrypt
    stops serving the cross-signed Root YR, the bundle needs Root YR.
  - The gateway opens the traffic WebSocket directly:
    `wss://<cluster>.cloud.thethings.network:8887/traffic/eui-<16 hex>`.
    TTS (lorawan-stack v3.37, `semtechws/lbslns`) also serves `/router-info`
    (itself a WebSocket: one query, one answer), but it only returns this
    same address, so the gateway skips it.
  - The key goes in the WebSocket upgrade request as the `Authorization`
    header (TTS adds "Bearer " if it is missing).
  - Then `version` from the gateway, `router_config` from the server.
- **API key**: entered on the Config tab, stored in NVS (encryption off,
  ADR-0002), write-only in the API, never logged, never shown again. A
  leaked key can only connect as this gateway.
- **Messages**:
  - Up: `jreq` (join-requests), `updf` (data frames), `propdf` (proprietary),
    `dntxed` (a downlink was sent).
  - Down: `dnmsg` (class A); `router_config`. TTS picks the receive window
    itself and sends only that one (`RxDelay` 1, `RX1DR`, `RX1Freq`,
    `xtime`): the gateway then has no second window to fall back to. When a
    server sends both RX1 and RX2 (Station's form), RX2 is the fallback,
    the same mechanism as the local server's (ADR-0015).
  - Ignored without an error: every other message type. TTS turns time sync
    on when it gets `version`, so `timesync` messages can arrive.
  - Wire formats (TTS source and tests): EUIs as `HH-HH-HH-HH-HH-HH-HH-HH`;
    `MIC` and `DevAddr` as signed 32-bit integers read little-endian (a value
    with the top bit set is negative); `xtime`, `diid` and `rctx` as exact
    64-bit integers, never through a double. A `dntxed` echoes the
    `DevEui` string of its `dnmsg` as it came.
  - Not used: class B (`dnsched`, beacons; no GPS), `timesync`, `runcmd`,
    `rmtsh` (remote commands are refused).
- **Channel plan**: the server's plan in `router_config` (frequencies and
  channels only) becomes the radio's when it passes the EU868 checks
  (ADR-0017); the radio restarts only when the plan differs. An unusable
  plan is logged and shown as `plan=differs` in the status line (console
  `STATUS`, `/api/status` `lbs`), and the radio keeps its plan.
- **Reconnect**: after a failure the client waits 10 s, doubling to 5 min; a
  refused key (HTTP 401/403) waits 5 min at once. A good `router_config`
  resets the wait.
- **Duty cycle and power**: in Basics Station the gateway enforces the duty
  cycle (Semtech protocol doc). Our radio task already does: the JIT queue,
  the per-sub-band budget and the 13 dBm cap (ADR-0007) apply to TTN
  downlinks and local downlinks alike.
- **Timestamps**: Basics Station times downlinks with `xtime`: bits 55-48 a
  session number (never 0, new at every concentrator start), bits 47-0 the
  concentrator microsecond counter. The concentrator counter is 32 bits; the
  client extends it, and must see the counter at least every 35 minutes
  (2^31 us) to count its wraps. TTS keeps its own count of wraps, so a
  downlink's bits 32-47 may differ from ours: the client checks only the
  session bits and takes the transmit time from the low 32 bits
  (xtime + RxDelay seconds). A downlink from another session is refused.
- **Transmit settings**: `dnmsg` carries no power, coding rate or
  polarity. The client uses what TTN sent over UDP before: EU868 power
  16 dBm (27 dBm in 869.4-869.65 MHz), coding rate 4/5, inverted
  polarity, no CRC, preamble 8; the radio's power cap (ADR-0007) applies.
- **Not supported**: class B (`dC` 1, `gpstime`: no GPS), and class C
  downlinks that do not answer an uplink (no `xtime`).
- **CUPS** (remote configuration and updates) is out of scope: firmware
  updates already come through our own signed OTA (ADR-0008), and CUPS would
  need a second key with more rights.

## Consequences

- Traffic to TTN is encrypted and authenticated; downlinks are confirmed
  (`dntxed`); the TTN console shows the gateway as connected while the
  WebSocket is open (protocol `SEMTECHWS/LBSLNS`).
- Setting up TTN needs one more step than UDP did: an API key from the TTN
  console. The registration help on the Config tab says how.
- A server that speaks only Semtech UDP cannot be used.
- The client authenticates with a bearer key only. Servers that require TLS
  client certificates (common with ChirpStack's Basics Station bridge and
  AWS IoT Core for LoRaWAN) cannot be used until client certificates are
  added.
- TLS costs RAM (one connection, about 40 KB) and needs a set clock to check
  certificates; with no clock or no network, the local server keeps working.
- Local devices still do not appear in TTN: TTN sees the gateway only.
  Linking the local server into TTN's network (Packet Broker) needs an
  agreement with The Things Industries and is not planned.

## Answered while building

- Discovery: TTS serves `/router-info`, but the traffic path
  (`/traffic/eui-<EUI>`) can be used directly (see Connection).
- CA: the chain ends at ISRG Root X1 through a cross-sign (see Connection).

## Verification (2026-11-23, rev3 bench board, desk build)

The bench card is the US915 GL5712-UX (ADR-0015 Verification), so the FiPy
sat about 1 m away and transmit was capped at 0 dBm.

| What | Result |
| --- | --- |
| Host tests (`test_lbs`, TTS's own values), round-trip fuzz invariant | pass |
| TLS to `eu1.cloud.thethings.network:8887`, API key in the upgrade request | pass |
| `router_config`: region EU863, data rates and both radios as ours, 8 channels | pass |
| TTN console: gateway connected, protocol `SEMTECHWS/LBSLNS` | pass |
| Uplinks: `jreq` and `updf` from unknown devices reach TTN (console counts them) | pass |
| FiPy OTAA join through TTN: join-accepts in RX1 (868.3 SF9) and RX2 (869.525 SF12) | pass |
| Data downlinks and a confirmed uplink's ACK (RX1 and RX2), each with `dntxed` | pass (5/5) |
| Frames handed to the radio 35 ms before their start | pass |
| API key in the logs (key text or "Bearer") | not found |
| A downlink lost on air (weak US915 card) and repeated by the device | handled |

## Plan

1. Core (built): `lwc/lbs.h`, `version`, `jreq`, `updf`, `propdf`,
   `dntxed`, `dnmsg` (to radio requests), `router_config` (checked against
   EU868: data rates, radio and channel settings), the `xtime` clock. The
   JSON reader and writer of the UDP code moved to `lwc/json.h` and are
   shared. Host tests use TTS's own test values; a fuzz target
   (`fuzz_lbs`) checks that every uplink message rebuilds to the frame it
   came from, as the server rebuilds it.
2. Firmware (built): TLS WebSocket client (reassembling fragmented messages:
   `router_config` is several KB), reconnect with backoff (see Reconnect), the API key
   setting (never logged, also not by the WebSocket client's debug output),
   Config tab and status page; `dntxed` when the frame is on air (the radio
   task's "queued" result is not enough); feeding the `xtime` clock once a
   minute; on-board test against TTN. The UDP forwarder removed.
