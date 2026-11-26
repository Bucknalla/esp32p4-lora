# ADR-0024: The region as a gateway setting

- Status: accepted (R2: host-tested; on the board: EU868 unchanged, RU864 receive-only, TX refused).
  Extended by ADR-0025: the concentrator card setting, US915/AU915/CN470 (one
  sub-band, receive-only), EU433; the transmit chain is off outside EU868.
  ADR-0027 adds the transmit rules per region; only EU868 transmits still.
- Date: 2026-11-25

## Context

ADR-0023 added the RP002 tables and moved EU868 onto them, with the region
fixed. R2 makes the region a setting and serves the dynamic-channel regions
(EU868, AS923-1..4, IN865, KR920, RU864) in the local server and the Basics
Station client. US915 and AU915 (fixed channel plans) are R3.

Transmitting in a region needs more than RP002: duty cycle, dwell time,
listen-before-talk and power limits are national rules. The firmware has
them for EU868 only (ETSI sub-bands, ADR-0007). IN865 (865-867 MHz) and
RU864 (864-870 MHz) lie inside 863-870 MHz, so the EU868 sub-band check
alone would let them transmit under the wrong rules. AS923 and KR920 are
inside 902-928 MHz, where this bench must never transmit.

## Decision

- **Only EU868 transmits.** `lwc_region_tx_enabled` is true for EU868 only.
  The radio task refuses every frame in another region (`TX_REGION`) before
  the frequency and duty-cycle checks. The other regions are receive-only
  until their national rules are in place (R4).
- **The setting**: `region=<name>` on `/api/radio` (admin), NVS
  `radio/region`; absent means EU868, so an upgrade changes nothing. US915 and
  AU915 are refused. A change resets the stored plan and receive windows to
  the region's defaults and restarts the gateway. The Radio tab has a region
  picker, a "receive only" chip, and locks the transmit switches outside
  EU868.
- **The region is passed explicitly** in core: `lwc_ns_t` holds it
  (`lwc_ns_set_region`; `lwc_ns_init` sets EU868), and the plan, data-rate,
  size and Basics Station functions take it as an argument. No EU868-only
  wrapper is left, so no caller stays on EU868 by accident.
- **Default plans**: EU868 keeps its plan (TTN, unchanged). The others: their
  default channels only (RP002), on as few radios as fit (|IF| <= 400 kHz;
  IN865's defaults span 922.5 kHz, so two radios), std and FSK off. No
  frequency plans are typed from memory.
- **Plan check**: the region's default channels must be in the plan; the
  863-870 MHz band check stays for EU868. Band edges for the other regions
  are not in the reference dump and are not typed from memory (R4). The
  stored plan format (`LWC_PLAN_VERSION` 1) is unchanged.
- **Receive windows**: RX2 must be 125 kHz LoRa (DR0-DR5, the same rule as
  before for EU868). EU868 keeps its 863-870 MHz RX2 range; elsewhere only the
  region's default RX2 frequency is accepted (no band edges yet).
- **What the server code assumes**, checked by `test_region` for every
  dynamic region: DR0-DR5 are SF12-SF7 at 125 kHz, and RX1DROffset 0 gives
  the uplink's DR. So ADR and the RX2 downlink (DR = 12 - SF) are the same
  code in every region. The RX1 size now goes through the region's RX1 DR
  table.
- **LinkADRReq channel mask**: bits for the region's default channels plus
  the CFList channels, no longer a fixed 0x00FF. AS923 and RU864 have 2
  default channels: 0x00FF would name channels those devices do not have, and
  they refuse the whole command. This also fixes EU868 plans with fewer than
  5 CFList channels.
- **AS923 sizes**: the server does not know a device's dwell setting
  (TxParamSetupReq is not sent), so where TxParamSetupReq exists the 400 ms
  dwell sizes are used: smaller, they fit either way.
- **Basics Station**: the router_config `DRs` are kept and compared with the
  gateway's region (`lwc_lbs_router_drs_ok`); the `region` name is logged
  only (servers spell it in their own ways). AS923 DR0-6 equal EU868's, so a
  mismatch is caught by the plan check (default channels), as the tests show.
- **Sessions**: devices joined with an RX2 that is not valid in the region
  (joined in another region) are not served after the restart; they must
  join again. This is decided at each start, in RAM: back in the old region
  the session is served again, unless its record was written in between (a
  class change, a record migration), which keeps it not joined.

## Consequences

- EU868 is unchanged: the golden snapshot (ADR-0023) passes unchanged, and
  so do the existing tests.
- In the other regions the gateway receives and forwards uplinks (Basics
  Station, local server); joins are accepted by the local server, but the
  join-accept is refused at the radio, so devices cannot complete a join
  from this gateway.
- The 868 MHz card cannot receive AS923 or KR920 (its filters); IN865 and
  RU864 are within its band. A check of the card band against the region is
  R4.

## Open questions

- AS923: TxParamSetupReq (dwell time) and the RX1 DR floor (ADR-0023).
- Band edges, duty cycle, dwell and LBT per region (R4).
- IN865 DR7 RX1DROffset 1 and KR920 offsets 6-7 (ADR-0023), still -1.

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| `test_golden_eu868` | unchanged |
| `test_region`: only EU868 transmits; DR0-5 = SF12-SF7 at 125 kHz and RX1 offset 0 = the uplink DR in every dynamic region; default channels sorted | pass |
| `test_plan`: every dynamic region's default plan passes its check, holds its default channels, packs and unpacks; US915/AU915 refused; EU868's plan refused in other regions; IN865 on two radios | pass |
| `test_ns`: per region, join-accept DLSettings (region RX2 DR), RX2 settings, channel mask (EU868 0x00FF, 0x003F with 3 CFList channels; 2-default regions 0x0003), sizes (AS923 dwell), US915 refused | pass |
| `test_lbs`: IN865 data rates; TTN's EU868 router_config refused for AS923-1 (plan), US915 (DRs) | pass |
| Board (rev3 desk build, 0 dBm cap): upgrade from R1 | region EU868, the server's plan kept |
| Board, EU868: FiPy join and 2 uplinks at 2 dBm | joined in 5 s; join-accept sent at 0 dBm (capped); router_config DRs match EU868 |
| Board: `region=US915` | 400, not supported yet |
| Board: `region=RU864` | restart; default plan 868.9/869.1 MHz on one radio, RX2 869.1 MHz DR0; the FiPy's EU868 session not served ("must join again"); TTN's EU868 plan refused ("RU864 checks"); console `TX_TEST`: `TX_REGION`, nothing sent |
| Board: back to `region=EU868` | restart; the server's plan again; the FiPy's session served again |
