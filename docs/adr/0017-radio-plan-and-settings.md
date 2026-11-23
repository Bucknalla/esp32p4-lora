# ADR-0017: One radio plan, from the server, with settings on the Config tab

- Status: accepted (steps 1-5 and the plan editor built and tested on the board)
- Date: 2026-11-23

## Context

The radio's channel plan is fixed in the firmware, in two copies: the
concentrator set-up (`conc_hal.c`: radio centre frequencies, eight multi-SF
channels, the 250 kHz LoRa channel, FSK) and the local network server's
join-accept (`core/src/ns.c`: the CFList with 867.1-867.9 MHz). Transmit
(on/off, the power cap, local-server transmit) is set at build time.

Real Basics Station takes the channel plan from the network server
(`router_config`). The user asked for full control from the Config tab, with
the plan coming from the server and stored for standalone and offline use.

## Decision

- **One plan** (`lwc/plan.h`, core): radio 0/1 centre frequencies, eight
  multi-SF channels (enable, radio, IF offset), the LoRa "std" channel (enable,
  radio, IF, SF, bandwidth), the FSK channel (enable, radio, IF). The radio,
  the local server's CFList and the `router_config` check all read it.
- **Source** (in this order): the server's `router_config` when connected;
  the last plan stored in NVS; the built-in EU868 TTN plan (today's values).
- **From the server, frequencies and channels only.** Never the clock source,
  RSSI offset, radio type, TX gain table or power: those belong to the board.
- **Checks** (core, before any use):
  - Every channel in 863-870 MHz; each radio's channels inside its receive
    band (SX1301 HAL: |IF| + bandwidth/2 <= radio bandwidth/2, so |IF| <= 400
    kHz for 125 kHz channels, <= 375 kHz for 250 kHz).
  - The three EU868 default channels (868.1, 868.3, 868.5 MHz) present:
    every device uses them before and after a join.
  - The TX frequency range stays fixed in the firmware, whatever
    `freq_range` says.
- **Applying a plan**: the radio restarts (about 3 s) only when the plan
  differs. If `lgw_start` fails with the new plan, the radio goes back to the
  last good plan, and the rejected plan is remembered so that a reconnect
  does not restart the radio again.
- **Local devices** keep the channels from their join-accept (the server
  has no NewChannelReq): after a plan change they must join again. The Config
  tab says so.
- **Transmit settings** (admin, logged; `POST /api/radio` gain, tx, lns_tx;
  NVS `radio/gain`, `radio/tx`, `radio/lns_tx`):
  - Antenna gain (dBi): the power cap at the connector becomes the legal
    limit minus the gain: 14 dBm ERP = 16.15 dBm EIRP on most sub-bands,
    27 dBm ERP = 29.15 dBm EIRP in 869.4-869.65 MHz (ADR-0007). It can only
    go below the firmware ceiling (`CONFIG_GW_TX_MAX_DBM`, 13 dBm on USB
    power), never above it.
  - Gateway transmit on/off.
  - Local-server transmit on/off, off by default (it was a build-time
    approval gate).
  - The Nebra gain table was made for EU868 cards: with another card the
    real output power is unknown. The card says so.
- **Local RX1 delay and RX2** (frequency, data rate; `POST /api/radio`
  rx1_delay, rx2_hz, rx2_dr; NVS `radio/rx1`, `radio/rx2hz`, `radio/rx2dr`):
  sent in the join-accept (RxDelay, DLSettings RX2 data rate) and stored per
  device, so a change applies to new joins only. The join-accept itself uses
  the default RX2 (869.525 MHz, DR0): the device does not know the new values
  yet. RX2 must be in an EU868 sub-band the radio may transmit in.
  - Device record version 2 adds the three fields at the end; a version 1
    record is converted at boot (field by field from a frozen copy of the
    old layout, with the defaults) and written back. The `meta` record
    (JoinNonce, DevAddr allocator) keeps its own version 1: bumping it would
    reset the JoinNonce and reuse values.
- **Plan editor** (Config tab, admin; `POST /api/radio/plan`: r0, r1,
  m0..m7, std, fsk, or reset): stored as `local` and the radio restarts.
  A connected network server's plan replaces it at the next connection.
- **SX1301 only**: the SX1302 path is still a start-up check.

## Verification (2026-11-23, rev3 bench board)

| What | Result |
| --- | --- |
| Host tests (`test_plan`, `test_lbs` with TTN's real router_config) | pass |
| Step 1: the HAL set-up from the plan is the one before (chains 0-8; FSK off) | pass |
| Step 2: `GET /api/radio`, Radio card | pass |
| Step 3: TTN's plan (the default plus FSK 868.8 MHz) stored, one radio restart, FSK chain on | pass |
| After a reboot: plan from NVS, the same router_config, no restart | pass |
| FiPy local join after the restart (hybrid) | pass |
| A plan the radio cannot start with: back to the last good one | not tested on the board (no valid plan makes lgw_start fail) |
| Step 4: defaults (3.0 dBi, cap 0 dBm on the desk ceiling, both TX on) | pass |
| Gain out of range or malformed: refused (HTTP 400) | pass |
| Gain 15 dBi: 16.1 - 15 = 1 dBm, held at the 0 dBm ceiling; radio restarts | pass |
| Local transmit off: FiPy join logged as a dry run (in hybrid TTN answered it) | pass |
| Gateway transmit off: local join-accepts refused (`TX_OFF`), no join | pass |
| Step 5 migration: FiPy joined on version 1 firmware, new firmware flashed: "record version 1 -> 2"; FCnt 2-3 accepted without a new join; ACK at RX1 1 s received | pass |
| RX1 2 s, RX2 DR3: join-accept carries them; ACK queued 1.98 s after the uplink and received; `/api/devices` shows them; kept after a reboot | pass |
| RX settings out of range (delay 0): refused | pass |
| Plan editor: 868.1 MHz removed, IF 450 kHz, std SF6: refused with the reason; FSK off: stored as local, radio restarted | pass |
| In hybrid mode after a reboot TTN's plan replaces the local one (one restart); reset: built-in plan, radio restarted | pass |

TTN's router_config (eu1, captured on the board) is in `test_lbs.c`.

## Plan

1. Core plan type, checks and CFList; `conc_hal.c` and `ns.c` read the
   default plan. No change in behaviour (FiPy join on the board, and
   `router_config` still "as ours").
2. NVS store; read-only Radio card and `GET /api/radio`.
3. Apply `router_config` (test vector: the real message captured from TTN);
   fallback to the last good plan; ADR-0016 updated.
4. Transmit settings.
5. Local RX1 delay and RX2 (device record version and migration), and the
   plan editor (built).
