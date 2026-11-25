# ADR-0026: Local network server in US915 and AU915

- Status: accepted (R3b: host-tested; on the board the server starts in US915
  and refuses wrong RX2 settings; no frame to serve, and no transmit, at
  902-928 MHz here)
- Date: 2026-11-25

## Context

ADR-0025 gave the fixed-channel regions one sub-band on the radio and
forwarded their frames to a network server only. The local server assumed
the dynamic-channel regions in several places:

- an uplink DR is 12 - SF at 125 kHz;
- RX1 is on the uplink's frequency at the uplink's SF and bandwidth;
- RX2 is at 125 kHz;
- the join-accept CFList lists frequencies;
- LinkADRReq uses ChMaskCntl 0 with one 16-bit mask.

None of these holds in US915 or AU915:

- US915 DR0 is SF10;
- RX1 is on downlink channel (uplink channel % 8), at 500 kHz;
- RX2 is 923.3 MHz at DR8 (SF12, 500 kHz);
- a device starts with all 72 channels on, and the gateway hears 9 of them.

## Decision

- **Regions served:**
  - Served: the dynamic regions, US915 and AU915 (`lwc_ns_region_ok`).
  - CN470 stays unserved: its original plan has no CFList, and moving a
    device to one sub-band takes 6 LinkADRReq, which do not fit in FOpts.
- **The sub-band** is the radio plan's (`lwc_plan_subband_of`), read again
  before each join and each answer. A sub-band change restarts only the
  radio, so the server would otherwise keep the old one.
  - If the plan is not one full sub-band, joins are refused with
    `LWC_NS_JOIN_NO_PLAN`. The log points to the Radio tab, and the DevNonce
    is not used up.
  - When the sub-band changes, every device gets the pair again. A device
    still on all 72 channels is then moved to the new sub-band. Devices
    already on the old one are no longer heard and must join again, and the
    Radio tab says so.
- **Join-accept:**
  - LoRaWAN 1.0.4 devices get the CFList type 1 (channel masks) for the
    sub-band.
  - `LWC_NS_MAC_1_0_2` devices (1.0.2 and 1.0.3) get no CFList: 1.0.2 has
    none in US915.
  - The masks match the reference (`region_ref.inc`). DLSettings carries
    RX2 DR8.
- **The sub-band pair:**
  - Until the device accepts it, every answer carries the two LinkADRReq
    of the reference: ChMaskCntl 7 (125 kHz channels off, the sub-band's
    500 kHz channel on), then the 16-channel block with its 8 channels.
  - **Both or neither.** The first command alone would leave no 125 kHz
    channel. If LinkCheckAns and DeviceTimeAns leave fewer than 10 bytes of
    FOpts, the pair waits for the next uplink.
  - **DR and TXPower fields:**
    - An ADR change, when ADR wants one: it is folded into the pair.
    - Otherwise 0xF, "keep", for 1.0.4 devices.
    - Otherwise the device's own DR and power, for 1.0.2 devices (0xF is
      not defined there).
  - **Accepted** only when every status bit is set. A device that refuses
    one field applies none of the block (LoRaWAN 1.0.4, LinkADRReq).
    - A device with ADR off still answers ALL_ACK. LoRaMac-node, the
      reference device stack (`RegionCommonLinkAdrReqVerifyParams`), uses
      its current DR and power then, and checks those.
    - A refusal or a lost answer waits before the next try: 4 uplinks,
      doubling per refusal up to 64.
  - **Separate from ADR.** This state is apart from ADR's, so a refused
    pair does not cap ADR. It is kept in RAM, so after a restart the pair is
    sent once more (harmless).
- **ADR** in US915 and AU915:
  - It works on the 125 kHz uplink DRs: DR0-3 (SF10-7) in US915, DR0-5
    (SF12-7) in AU915. Each is one SF step, which `test_region` checks.
  - The demodulation floor comes from the DR's SF in the region table.
  - After the pair, a LinkADRReq carries the sub-band's block mask
    (ChMaskCntl 0-3), which leaves the other blocks unchanged.
- **Downlinks** (`lwc_ns_downlink`, `lwc_ns_downlink_at`) take the region:
  - **RX1:** the region's RX1 DR at RX1DROffset 0, and its RX1 frequency.
    For example, SF10 at 125 kHz on 903.9 MHz is answered with SF10 at
    500 kHz on 923.3 MHz.
  - **RX2:** a DR the server accepts for RX2, otherwise the region's
    default RX2 DR.
  - **Class C:** the airtime uses the same SF and bandwidth.
  - **EU868:** the golden snapshot is unchanged.
- **Receive windows:**
  - In US915 and AU915, RX2 must be a downlink DR (DR8-13) at 923.3 MHz.
  - The Radio tab offers DR8-13 there and hides the form only for CN470
    (JSON flag `local`).
- **Transmit:** unchanged. Nothing transmits outside EU868 on the 868 card
  (ADR-0024, ADR-0025). In US915 the server builds join-accepts and answers,
  and the radio refuses them (`TX_REGION`).

## Consequences

- The local server is ready for US915 and AU915, but it cannot complete a
  join until those regions may transmit (R4).
- In hybrid mode in US915 and AU915, a join from a device registered here is
  now taken by the local server. It is stored and uses up a DevNonce, but it
  is not answered (the radio refuses to transmit) and not forwarded, as in
  the other receive-only regions since ADR-0024.
- None of this was tested over the air. The bench may not transmit at
  902-928 MHz, and no device here can.

## Open questions

- **Which TxParamSetupReq and dwell sizes to use.** AU915 has
  TxParamSetupReq, so `lwc_ns_max_frm` uses the 400 ms dwell sizes, as in
  AS923. Sending TxParamSetupReq is R4.
- **CN470 in the local server.** This needs port-0 MAC commands or the
  RP002-1.0.1+ plans.

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| `test_golden_eu868` | unchanged (`lwc_ns_adr` and `lwc_ns_downlink` now take the region) |
| `test_ns` US915/AU915 set-up | region served, sub-band 2 (index 1) from the default plan; RX2 accepted only as DR8-13 at 923.3 MHz |
| `test_ns` join | 1.0.4: 33-byte join-accept, CFList equal to the reference masks; 1.0.2: 17 bytes, no CFList; a plan that is not one sub-band gives `JOIN_NO_PLAN`, and the same DevNonce joins once the plan is fixed |
| `test_ns` sub-band pair | first uplink: the pair as the reference (1.0.2 with its DR and power, 1.0.4 with 0xFF); refused: sent again after 4, then 8 uplinks, ADR not capped, a late answer not taken; accepted: no more pairs |
| `test_ns` sub-band change | a new plan's sub-band: the pair again with the reference masks of the new sub-band; the same plan again resets nothing |
| `test_ns` FOpts room | LinkCheckAns + DeviceTimeAns leave no room: no half pair |
| `test_ns` ADR | after the pair, a block-mask LinkADRReq up to the top 125 kHz DR; before it, ADR folded into the pair |
| `test_ns` RX1 | SF10 at 125 kHz on channel 8 → 923.3 MHz SF10 at 500 kHz; channel 9 at SF7 → 923.9 MHz; the 500 kHz channel at SF8 → 923.9 MHz SF7 |
| `test_ns` RX2 and class C | RX2 SF12 at 500 kHz; a bad RX2 DR gives DR8; class C at RX2 DR10 → SF10 at 500 kHz |
| `test_ns` sizes | every RX1 and RX2 DR has a size above 0 |
| `test_ns` CN470 | not served |
| `test_region` `fixed_rules` | DR0..up125.max_dr one SF step at 125 kHz; RX1 at offset 0 is a downlink DR for every uplink DR; RX2 inside DR8-13; downlink DRs LoRa at 500 kHz |
| cppcheck; firmware desk build | clean; builds |
| Board, EU868 | FiPy at 2 dBm: joined in 5 s (join-accept RX1 868.5 MHz SF7), 2 uplinks |
| Board, 915 card, US915 | local server starts (no "not served" warning), `local: true`; RX2 DR10 at 923.3 MHz accepted, DR3 refused; back to EU868 with the session in use |
