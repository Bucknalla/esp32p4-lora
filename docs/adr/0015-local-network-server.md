# ADR-0015: Local network server and gateway modes (phase 3)

- Status: accepted (slices 1–6 built; modes and desk tests on the board with
  a real device; transmit off by default, on only in a desk build; see
  Verification)
- Date: 2026-11-22

## Context

Phase 3 of the design plan makes the gateway a stand-alone LoRaWAN network:
network server and join server on the P4. The user asked for a gateway mode
on the Config tab: forward to TTN, standalone, or hybrid. Until now the only
mode was the Semtech UDP forwarder (ADR-0012), switched on or off.

## Decision

- **Modes** (Config tab, admin; replaces the `pktfwd/ttn` on/off setting,
  which is migrated):
  - **TTN**: every frame goes to TTN (Semtech UDP at first; Basics Station since ADR-0016).
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
2. Core join and uplink decisions over a fixed-size device table (`lwc/ns.h`). Done.
3. Firmware: router between radio, local server and forwarder; mode setting;
   persistence; TX path (dry run). Done (`lns.c`, `gwmode.c`).
4. API and UI: mode selector and devices on the Config tab. Done.
5. Class A answers: ACK, LinkCheck, DeviceTime, DevStatus, RX2 fallback.
   Done (`lwc/mac.h`, `lwc_ns_answer`).
6. ADR (LinkADRReq). Done (`lwc_ns_adr`).
Then integrations (phase 4).

## Consequences

- The gateway cannot be tested end to end until there is an EU868 OTAA device
  and transmit approval; until then, host tests and dry-run logs.
- Standalone mode stops the gateway serving the community network; hybrid
  keeps it.

## Notes from slice 2

- **DevNonce**: devices are added as LoRaWAN 1.0.4 (DevNonce is a counter and
  must increase) or 1.0.2/1.0.3 (random DevNonce; the last 8 are refused).
  Many devices in use are 1.0.2/1.0.3.
- **JoinNonce** is one counter for the gateway and must be stored: 1.0.4
  devices refuse a join-accept whose JoinNonce is not greater than the last.
- **Rejoin** keeps the device's DevAddr and resets its counters.
- **Uplink**: frames are tried against every joined device with that DevAddr;
  an authentic frame with an old counter is a replay (dropped, not forwarded).
  FRMPayload on port 0 is decrypted with the network key.
- **Hybrid**: "handled locally" means an accepted join or uplink, or a replay
  of one; a bad MIC is not, so it is forwarded.

## Notes from slices 3 and 4

- **Mode setting**: NVS `pktfwd/mode` (0 TTN, 1 standalone, 2 hybrid), read
  once at boot. The old `pktfwd/ttn` value is migrated on first read: on →
  TTN, off → standalone. `POST /api/settings mode=...` replaces `ttn=on|off`.
- **Routing**: TTN mode keeps the radio queue → forwarder path unchanged.
  Standalone and hybrid start the `lns` task (core 1, priority 4, below the
  radio); hybrid gives the forwarder a second queue with every frame the
  local server did not handle.
- **Storage** (NVS namespace `lns`): `meta` (JoinNonce, DevAddr allocator),
  `dNN` (device record with keys, versioned), `cNN` (counters, 12 bytes,
  written on every accepted uplink: one small entry, so flash wear stays low).
  On a join the device record and `meta` are written before the join-accept
  is queued; if that fails, the join is not answered. The downlink counter
  skips 16 at boot.
- **Transmit**: requests carry a source tag; the radio task logs the result of
  local-server frames instead of putting it on the forwarder's result queue.
  `CONFIG_GW_LNS_TX` is off by default: the join-accept is logged as a dry run.
  In dry run a join is not stored, the device list says "join request seen,
  not answered", and in hybrid mode the join-request still goes to TTN, so a
  device registered on both can keep joining TTN.
- **API**: `GET /api/devices` (viewer, no keys), `POST /api/devices` and
  `POST /api/devices/delete` (admin). Devices can be added in every mode.
- RSSI and payloads shown in the UI are RAM only.

## Notes from slice 5

- **When a device gets a downlink**: a confirmed uplink (ACK), ADRACKReq set
  (an empty frame is enough), LinkCheckReq, or DeviceTimeReq when the wall
  clock is set. Nothing else causes a transmission. Answers go in FOpts (at
  most 15 bytes), never on port 0. DevStatusReq rides along at most once a day,
  only on a downlink that is sent anyway; the battery and margin show on the
  Config tab (RAM only).
- **Repeated uplinks**: an authentic frame with the same counter as the last
  one is a repeat (a device that missed its ACK, or NbTrans > 1), not a
  replay. It gets its ACK again with a new downlink counter; its payload is not
  delivered twice and its MAC commands are not read twice. After 7 repeats it
  is a replay (dropped), so a recorded frame cannot make the gateway transmit
  without limit. An older counter is read as the next 16-bit epoch and fails
  the MIC (`BAD_MIC`; in hybrid mode it goes to TTN).
- **DeviceTimeAns**: GPS time = Unix time - 315964800 + 18 leap seconds
  (checked against the GPS week 2048 rollover, 2019-04-06T23:59:42Z). The time
  is when the uplink reached the radio task, from the SNTP clock; no answer
  without a set clock.
- **LinkCheckAns**: margin = uplink SNR minus the demodulation floor for its
  SF (SF7 -7.5 dB ... SF12 -20 dB), gateway count 1.
- **Counters**: one NVS write per accepted uplink holds both counters, and it
  happens before the answer is queued; if it fails, the answer is not sent.
  The log says how many milliseconds after the uplink the answer was queued
  (RX1 opens at 1 s).
- **RX2 fallback**: each local-server request carries its RX2 window (one
  second after RX1, 869.525 MHz SF12). If the radio task refuses RX1 (too late,
  collision, duty-cycle budget), it tries RX2 once, in the radio task, so no
  result has to travel back to the server. Join-accepts use it too (RX2 at 6 s).

## Notes from slice 6

- **Algorithm**: ChirpStack's default ADR (`chirpstack/src/adr/default.rs`,
  read for this slice). Take the best SNR of the last 20 uplinks with the ADR
  bit (125 kHz only), subtract the demodulation floor of the data rate in use
  and a 10 dB installation margin, divide by 3 dB and truncate toward zero.
  Each spare step raises the data rate up to DR5 (SF7), then lowers the power
  by one TXPower index (2 dB); a shortfall only raises the power, and only
  once the history is full at the current power (so the power does not go up
  and down). Faster and quieter can come from the first uplink. The device
  lowers its own data rate when it hears nothing (ADRACKReq), so the server
  never does.
- **Differences from ChirpStack**:
  - NbTrans stays 1 (ChirpStack sets it from packet loss).
  - The history starts again when the power changes, instead of being
    filtered by power.
  - After a refused or unanswered LinkADRReq the server waits 20 uplinks
    before asking again.
  - TXPower index 5 at most (MaxEIRP - 10 dB). RP002 allows 7; 5 is kept for
    devices on older regional parameter versions, and a device that refuses
    an index gets a lower cap.
  - A device that accepts a data rate but sends its answer at a lower one
    (its application fixes the rate): the first time, the server waits 20
    uplinks; the second time in a row, it caps the data rate at the rate the
    device uses, until it joins again. Without this, such a device got the
    same request on every uplink (seen on the desk).
- **Answers**: the next uplink must carry LinkADRAns. All three bits set: the
  new power is recorded. A refusal lowers the cap for what was refused (power
  or data rate). Downlinks set FCtrl.ADR. ChMask keeps channels 0-7 (the
  three defaults and the CFList).
- **Not sent**: DutyCycleReq, RXParamSetupReq, NewChannelReq and
  RXTimingSetupReq. The channels and RX settings come in the join-accept and
  do not change.
- **After a restart** the ADR state is gone (RAM only): the server assumes
  index 0 and relearns.
- Every LinkADRReq is a transmission, so it is also dry run until
  `CONFIG_GW_LNS_TX` is approved.

## Verification

Desk tests (2026-11-23) used the bench card, a MaxIIoT GL5712-UX: the US915
variant, whose filters take about 60 dB off 868 MHz. The FiPy sat about 1 m
away; local transmit was on in a desk-only build with the power cap at 0 dBm
(`CONFIG_GW_LNS_TX=y`, `CONFIG_GW_TX_MAX_DBM=0`). Downlinks at SF7 arrived
near the FiPy's limit (SNR -7 dB), so some were lost; SF9 was reliable.

| What | How | Result |
| --- | --- | --- |
| AES, CMAC | FIPS-197, RFC 4493 vectors | pass |
| Frames, keys, MICs | ChirpStack `lrwn` vectors; the downlink rebuilt byte for byte | pass |
| GPS time | 2019 GPS week rollover (week 2048 at 23:59:42 UTC) | pass |
| Joins, uplinks, repeats, answers, ADR | host tests (`test_ns`, `test_mac`), ASan/UBSan | pass |
| Untrusted input | libFuzzer on frames, MAC commands and the answer path (CI) | CI |
| Static checks | cppcheck; core with GCC and the CI warning flags | clean |
| Standalone boot (rev3 bench board) | local server on, no forwarder, dry run | pass |
| Add a device (Config tab) | `devices=1` in STATUS | pass |
| Hybrid boot | device loaded from NVS, local server and TTN link up | pass |
| TTN boot | device loaded, local server off, TTN link up | pass |
| Remove a device | logged, record erased | pass |
| Join on air | Pycom FiPy (LoRaWAN 1.0.0 stack), desk build at 0 dBm (below) | pass |
| Uplinks, confirmed uplink and ACK, repeat after a lost ACK | FiPy | pass |
| DevStatusReq/Ans | FiPy: battery shown. Margins seen: 26 and -30 dB at a downlink SNR near -7 to -9 dB (they look like SNR x 4, cut to 6 bits); shown as sent | pass |
| ADR power on air | FiPy accepted LinkADRReq and lowered its power (RSSI -94 to -111 dBm); a lost answer waits 20 uplinks | pass |
| ADR, device keeps its own rate | FiPy with a fixed DR: was one request per uplink; now a wait of 20 uplinks, and a cap at the rate it uses after the second time (desk run with the first form of the fix: 2 requests in 12 uplinks) | pass (fixed) |
| ADR data-rate change on air | the FiPy either fixed its rate or already sent at DR5 | not yet |
| Session after a gateway reboot | FiPy sent FCnt 4-6 with no new join; downlink counter skipped 16 | pass |
| Hybrid: local device not forwarded | FiPy frames handled locally, nothing of it sent to TTN | pass |
| Wall clock in standalone mode | SNTP started only with the forwarder: no clock after a cold boot; now started in every mode | pass (fixed) |
| RX2 fallback, DeviceTimeReq, LinkCheckReq | the FiPy firmware cannot send these, and RX1 was never refused | not yet |
| TTN downlinks after the `conc_tx.c` change | TTN join-accepts, data downlinks and ACKs over Basics Station (ADR-0016 Verification) | pass |
