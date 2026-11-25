# ADR-0023: Regional parameters (RP002 tables)

- Status: accepted (R1: tables and rules host-tested; EU868 moved onto them, byte-identical; no other region enabled)
- Date: 2026-11-25

## Context

The gateway is EU868 only: data rates, payload sizes, RX2, default channels
and duty-cycle bands are written into `ns.c`, `lbs.c`, `plan.c` and `txpk.c`.
The owner wants every RP002 region. The bench has an 868 MHz card and a
915 MHz card; nothing may transmit at 902-928 MHz, so the 915 card stays
receive-only until it is measured. EU868 and the UK come first.

Hand-typed regional tables are a known source of bugs. The fixed-channel
plans (US915, AU915) also differ by rule, not only by value: 64 + 8 uplink
channels, 8 downlink channels, RX1 on downlink channel (uplink % 8), and a
sub-band chosen by CFList or LinkADRReq.

## Decision

- **One table per region, generated** (`core/src/region_tables.inc`, by
  `tools/regions/gen_tables.py`) from a dump of
  `github.com/brocaar/lorawan/band` (MIT, the ChirpStack tables; version
  pinned in `tools/regions/banddump/go.mod`). Values are RP002-1.0.3 for
  LoRaWAN 1.0.4. Regions: EU868, US915, AU915, AS923-1..4, IN865, KR920,
  RU864.
- **Repeater-compatible sizes** (EU868 DR4-7: N = 222), which is what the
  gateway sends today; the 400 ms dwell sizes are kept beside them.
- **The fixed plans as a rule, checked at generation.** The generator
  asserts that the 72 uplink and 8 downlink channels lie on a regular grid
  and that RX1 is channel (i % 8); the C code computes channels from base and
  step (`lwc_region_channel`, `lwc_region_rx1_hz`) instead of listing 72.
- **Where the reference is wrong, refuse.** It fills RX1 DR rows for RFU
  uplink DRs (KR920 DR6 read as DR0 in every slot) and some targets that are
  RFU in the region (KR920 DR7, IN865 DR6). The generator writes -1 (not
  defined) there; `test_region` checks that no RX1 DR is RFU.
- **Downlink dwell in AS923**: with the 400 ms downlink dwell limit the
  RX1 DR is raised to at least DR2. The generator finds this floor from the
  reference's two tables and checks that it is only a floor
  (`rx1_dwell_min_dr`; 0 in the other regions); `lwc_region_rx1_dr` takes
  the device's dwell setting.
- **Sub-bands** (`lwc_region_cflist_subband`,
  `lwc_region_link_adr_subband`): a 1.0.3+ device gets a type 1 CFList
  (channel masks); a 1.0.2 device has no CFList for fixed plans and gets two
  LinkADRReq commands (ChMaskCntl 7 with the 500 kHz channel, then the
  16-channel block). Both are compared, for all 8 sub-bands of US915 and
  AU915, with the reference's own output (`core/test/region_ref.inc`).
- **EU868 uses the tables now**: `lwc_ns_dr`, `lwc_ns_max_frm`, `lwc_lbs_dr`,
  `lwc_lbs_dr_params`, the NS RX2 default and the plan's default channels.
  LR-FHSS DRs (EU868 DR8-11) give N = 0: the SX1302 cannot receive them.
- **Byte-identical, proven.** Before the change, `test_golden_eu868` wrote
  every output of the region-dependent functions over a grid of inputs
  (616 lines: DRs, sizes, RX settings, duty-cycle bands and a take sequence,
  the default plan and CFList, ADR steps, LinkADRReq bytes, class A windows,
  a join-accept, Basics Station windows) to `core/test/golden_eu868.txt`. It
  is a ctest and passes unchanged after the change.
- **Not in the tables**: duty cycle, dwell time and listen-before-talk. They
  depend on national rules (ETSI for the EU and UK) as well as RP002, and
  stay with the transmit path (`txpk.c`). `lwc_ns_rx_ok` keeps its EU868
  band limits for the same reason.

## Consequences

- Nothing changes on air: the region is fixed to EU868 in this step.
- Next steps, each its own PR:
  - R2: the region as a gateway setting; the dynamic plans (AS923, IN865,
    KR920, RU864) wired through the NS and Basics Station.
  - R3: US915 and AU915 (sub-band setting, join and ADR), tested receive-only
    on the 915 card.
  - R4: the card's band checked against the region; duty cycle, dwell and
    LBT per region; a Basics Station `router_config` region that does not
    match the gateway's region is refused.
- The 915 card's transmit path stays off until it is measured.

## Open questions

- KR920: RP002 defines RX1DROffset 0-5; the reference also fills 6 and 7.
  R2 should refuse offsets the region does not define rather than use them.
- IN865 uplink DR7, RX1DROffset 1: the reference gives DR6 (RFU in IN865);
  the table says -1. RP002 probably defines a value there (DR5): check it
  against the spec text in R2 and do not read -1 as the spec.
- US915 and AU915 keep RX1 rows for DR8-13, which are downlink only. They
  are harmless now (no uplink arrives at those DRs); mask them in R3.
- The spot values in `test_region` were typed by hand from RP002 values, not
  checked line by line against the RP002-1.0.3 document in this step.

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| `test_golden_eu868` (616 lines captured before the change) | unchanged |
| `test_region`: RP002 spot values for EU868, US915, AU915, AS923-1/2, KR920, IN865, RU864, typed by hand, independent of the generator; the AS923 dwell floor; lookup and bad input; every region's tables consistent (RX2 DR is LoRa, sizes, no RX1 DR to an RFU DR, every fixed-plan channel answers on its downlink channel); 32 sub-band rows (US915, AU915, 1.0.2 and 1.0.4) against the reference | pass, with ASan/UBSan |
| All host tests (22), macOS and Ubuntu 24.04 (clang); cppcheck 2.13; fuzz targets build | pass |
| Firmware build, rev3 and rev1 | pass |
