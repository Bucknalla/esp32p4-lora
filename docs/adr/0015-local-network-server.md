# ADR-0015: Local network server and gateway modes (phase 3)

- Status: accepted (slice 1 done: crypto and frame codec)
- Date: 2026-11-22

## Context

Phase 3 of the design plan makes the gateway a stand-alone LoRaWAN network:
network server and join server on the P4. The user asked for a gateway mode
on the Config tab: forward to TTN, standalone, or hybrid. Until now the only
mode was the Semtech UDP forwarder (ADR-0012), switched on or off.

## Decision

- **Modes** (Config tab, admin; replaces the `pktfwd/ttn` on/off setting,
  which is migrated):
  - **TTN**: every frame goes to TTN over Semtech UDP, as today.
  - **Standalone**: frames are handled by the local server only; nothing is
    forwarded.
  - **Hybrid**: a frame is handled locally only when it belongs to a local
    device *and its MIC passes*; everything else, including a DevAddr match
    with a bad MIC (an address collision), goes to TTN.
- **Protocol scope**: LoRaWAN 1.0.4 (L2 specification) with EU868 regional
  parameters (RP002-1.0.x), class A, OTAA. ABP only for tests. Join and
  receive delays, RX2 (869.525 MHz) and the CFList for channels 4–8 come from
  RP002; the gateway's eight uplink channels are the TTN EU868 plan.
- **Addresses**: NetID 0x000000 (type 0, reserved for private and
  experimental networks), so DevAddrs are 0x00000000–0x01FFFFFF. These never
  collide with TTN's (NetID 0x000013, DevAddr prefix 0x26/0x27).
- **Crypto in the core**: software AES-128 (encrypt and decrypt: the server
  builds join-accepts with AES decrypt) and AES-CMAC, OS-independent and
  host-tested. The load is a few frames a minute, so the P4's AES hardware is
  not needed. Table-based AES is not hardened against cache timing, which needs
  code on the same CPU.
- **Tests**: AES against FIPS-197, CMAC against RFC 4493, frames against
  ChirpStack's `lrwn` vectors (join-request MIC, encrypted 1.0 join-accept,
  1.0 session keys, a 1.0 uplink with FOpts and FRMPayload, a downlink MIC).
  A fuzz target covers the frame parser in CI. Tests use published test keys
  only.
- **Transmit**: local downlinks (join-accepts first) use the existing JIT
  queue, duty-cycle budget and 13 dBm cap (ADR-0007), with their own result
  path so they cannot take the forwarder's results. Until the user approves
  transmitting, the local server runs **dry**: it logs the join-accept it would
  send and does not transmit.
- **Keys and counters** (NVS, encryption off per ADR-0002): AppKeys are
  write-only in the API, admin only, never logged (viewers can read the log
  ring). DevNonce is stored before the join-accept is sent; the downlink
  counter skips ahead at boot; uplink counter writes are batched.

## Slices

1. Core crypto and LoRaWAN 1.0.x codec (`lwc/aes.h`, `lwc/lorawan.h`). Done.
2. Core join and uplink decisions over a fixed-size device table.
3. Firmware: router between radio, local server and forwarder; mode setting;
   persistence; TX path (dry run).
4. API and UI: mode selector and devices on the Config tab.
Then downlinks (ACK, MAC commands), ADR, and integrations (phase 4).

## Consequences

- The gateway cannot be tested end to end until there is an EU868 OTAA device
  and transmit approval; until then, host tests and dry-run logs.
- Standalone mode stops the gateway serving the community network; hybrid
  keeps it.
