# ADR-0027: Transmit rules per region, and TxParamSetupReq

- Status: accepted (R4: host-tested; on the board, EU868 unchanged. No region
  other than EU868 transmits.)
- Date: 2026-11-25

## Context

Before R4, the transmit path knew only EU868: the ETSI sub-bands and their
duty cycle (`lwc_dc_*` in `txpk.c`), and the 863-870 MHz check. Every other
region is receive-only (ADR-0024, ADR-0025). The owner chose "rules only" for
R4:

- add the rules for each region: bands, power, duty cycle, dwell time, LBT
  and TxParamSetupReq;
- keep transmit enabled only for EU868 on the 868 MHz card. Another region
  may transmit later, one region at a time, and only with the owner's
  approval.

## Decision

- **The gate is unchanged.** `lwc_region_tx_enabled` (EU868 only) and the two
  firmware gates (`conc_hal.c`, `conc_tx.c`) stay as they were. A complete
  rule row does not open the gate. `test_txrule` checks two things: the gate
  is open only for EU868, and EU868 has a complete row.
- **One table, `lwc/txrule.h`.** Each region has a row with:
  - the bands where the gateway may transmit (channel centre frequency,
    `[lo, hi)`);
  - for each band, the duty cycle per hour and the highest TX power;
  - a duty cycle per downlink channel;
  - LBT: whether it is required, the threshold and the listen time.
- **Rows are taken from pinned sources only:**
  - LMN, LoRaMac-node v4.7.0 `src/mac/region/Region*.{h,c}`: the device
    frequency limits (`VerifyRfFreq`), the band duty cycles, and the LBT
    threshold and listen time.
  - BS, Basics Station v2.0.6 `src/s2e.c` and `src/sx130xconf.c`: the
    gateway duty cycle, the default TX power and the gateway LBT.
  - RP, RP002 max EIRP (`region_tables.inc`).
  - Where two sources differ, the row takes the stricter value.
  - A region without a sourced band list has no row (`n_bands` 0), and the
    check refuses it (`LWC_TXR_NO_RULES`).
- **The rows:**

  | Region | Bands (centre) | Duty cycle | Power | LBT | Source |
  | --- | --- | --- | --- | --- | --- |
  | EU868 | the 6 ETSI sub-bands, as before | 0.1 / 1 / 10 % per band | 16 dBm; 27 dBm in 869.4-869.65 | no | LMN, BS |
  | US915 | 902.3-927.5 MHz | none | 26 dBm (BS; RP 30) | no | LMN, BS |
  | AU915 | 915.2-927.8 MHz | none | 30 dBm | no | LMN, BS, RP |
  | IN865 | 865-867 MHz | none | 30 dBm | no | LMN, RP |
  | RU864 | 864-870 MHz | 1 % | 16 dBm | no | LMN, RP |
  | EU433 | 433.175-434.665 MHz | 1 % | 12 dBm | no | LMN, RP |
  | KR920 | 920.9-923.3 MHz | 2 % per channel | 10 dBm below 922.1 MHz, 14 dBm from it (LMN; BS 23) | -67 dBm (BS; LMN -65) for 6 ms (LMN; BS 5 ms) | LMN, BS |
  | AS923-1 | none: the band depends on the country | 10 % per channel | - | -80 dBm for 5 ms | LMN, BS |
  | AS923-2/3/4 | none: no gateway source | - | - | - | - |
  | CN470 | none: LMN v4.7.0 has only the RP002-1.0.1+ plans | - | - | - | - |

- **The check** is `lwc_txrule_check(rule, hz, have_lbt, &max_dbm)`. It
  returns one of:
  - `NO_RULES`: no band list;
  - `FREQ`: outside every band;
  - `LBT`: LBT is required and the hardware cannot do it;
  - `OK`: the result also gives the band's highest power.
- **Firmware (`conc_tx.c`):**
  - The check replaces the old EU868 band check.
  - `FREQ` maps to `TX_FREQ`, as before. `NO_RULES` and `LBT` map to
    `TX_REGION`.
  - The band power only lowers the gateway's own cap: the gain-table cap and
    the antenna ceiling of ADR-0017. With the EU868 powers (16 and 27 dBm),
    the cap does not change. (The band power is EIRP and the cap is conducted
    power: see the open questions.)
- **No LBT on this hardware.** In lora_gateway v5.0.1, SX1301 LBT
  (`loragw_lbt.c`) needs a Semtech FPGA with the LBT feature, and an SX127x
  to scan. Cards like the GL5712-UX have neither. The firmware therefore
  passes `have_lbt = false`, so KR920 is refused with `LBT`. No LBT is
  implemented.
- **Duty cycle (`lwc_dc_*`)** takes the rule:
  - a token bucket per band, as before;
  - for KR920 and AS923-1 (BS), also one bucket per downlink channel: up to
    16 channels; when the list is full, the frame is refused.
  - **Region changes.** The budgets are kept once per boot, as before. If
    the region changes after a frame was sent, the budgets start empty, so a
    region change cannot refill them. If no frame was sent yet (for example,
    the gateway booted in another region and was then set to EU868), they
    start full, as before R4. Before R4, a change away from EU868 and back
    kept the EU868 budgets; now they start empty, which is stricter.
- **Gateway dwell time: none.** Neither BS nor LMN has a dwell limit for
  gateway transmissions. LMN notes that ARIB STD-T108 does not require dwell
  time with LBT. Dwell time is a device setting, sent with TxParamSetupReq.
- **TxParamSetupReq:**
  - The server sends it in the regions that define it (`tx_param_setup`:
    AS923-1..4 and AU915), with the region's defaults (LMN v4.7.0):
    - uplink dwell on;
    - downlink dwell off;
    - MaxEIRP set to the region's max EIRP: 16 dBm (index 5) for AS923, 30 dBm
      (index 13) for AU915.
  - For a device with the LoRaMac-node defaults, these values change
    nothing, and the ADR power indices do not move. What the command adds is
    that the server learns the device's downlink dwell. A device with other
    defaults takes these values.
  - **AS923:** sent to all devices. The pinned brocaar/lorawan band package
    (`ImplementsTXParamSetup`, the reference of ADR-0023) has the command in
    AS923 for every LoRaWAN version.
  - **AU915:** sent only to 1.0.4 devices, and only after the sub-band pair
    is accepted (ADR-0026).
    - It never goes in the same frame as the pair, so the pair still has its
      10 bytes and stays both-or-neither.
    - `LWC_NS_MAC_1_0_2` devices (1.0.2 and 1.0.3) in AU915 do not get it.
      brocaar has no such command in AU915 for 1.0.1 and 1.0.2, and an
      unknown command ends the device's parsing of the rest of FOpts. 1.0.3
      has the command, but this setting does not tell 1.0.3 from 1.0.2.
  - **Answer:** TxParamSetupAns (no payload) in the next uplink means the
    device took the settings. If no answer comes, the server waits 4 uplinks
    before it sends the command again, and doubles the wait each time, up to
    64 uplinks (the R3b back-off).
  - **Sizes:** before the answer, and after a restart or a new join, the
    server keeps the smaller 400 ms dwell sizes (`lwc_ns_max_frm`). After the
    answer, it uses the sizes without the dwell limit (`lwc_ns_dev_max_frm`):
    for RX1, RX2, class C and the downlink queue.
  - **RX1 DR:** the RX1 DR stays computed with downlink dwell off, as before.
    This is the device default, and it is the value the command sends.

## Consequences

- EU868 transmits exactly as before. The golden snapshot is byte-identical:
  it records the band of each frequency from 862.9 to 870.1 MHz in 50 kHz
  steps, each band's duty cycle, and 60 budget takes. `test_txrule` checks
  that the frequencies the firmware uses are accepted, with the same power
  limits.
- In AS923, each new session gets one more downlink, which carries the
  TxParamSetupReq. In AU915 this happens after the sub-band pair. While these
  regions are receive-only, those frames are refused at the radio, like
  every other frame there.
- When a region is later allowed to transmit, the rules, the duty cycle and
  the MAC command are in place and host-tested. The gate is not the only
  change: the power cap must also change (see the open questions).

## Open questions

- **AS923 bands and LBT by country.** AS923-1 covers countries with and
  without LBT, and with different bands. The row keeps the strictest values
  (LBT and 10 % per channel) and has no band list. A country setting is not
  added.
- **The power cap is still the EU868 cap.** `radioplan_tx_cap_dbm`
  (ADR-0017) is the EU868 limit (16.1 dBm EIRP) minus the antenna gain:
  conducted power. `conc_tx.c` compares the band power (EIRP) with it
  directly. For EU868 the band powers are at or above that limit, so nothing
  changes. Before another region transmits, the band power must become a
  conducted limit (minus the antenna gain) in place of the EU868 constant.
- **Band limits are centre frequencies**, as in the sources. The card check
  (ADR-0025) keeps every channel's edges inside the card's band, but these
  rows do not check the edges against the regulatory band.
- **Before any region beyond EU868 may transmit:**
  - the transmit power of the card for that band must be measured;
  - its row must be checked against the national rules;
  - the owner must approve it.
- **The 915 MHz card does not transmit.** The bench rule for 902-928 MHz
  stays.

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| `test_golden_eu868` | byte-identical (the duty-cycle bands and takes now go through the EU868 rule) |
| `test_txrule` EU868 | 868.1-868.5, 867.1-867.9, 868.8 and 869.8 MHz: OK at 16 dBm; RX2 869.525 MHz: OK at 27 dBm; gaps and band ends refused |
| `test_txrule` gate | `lwc_region_tx_enabled` only for EU868; EU868 has a full row without LBT, and its RX2 is accepted |
| `test_txrule` rows | every row with bands holds the region's RX2 and default (or downlink-grid) channels; power 1-30 dBm; duty at most 10 % |
| `test_txrule` unsourced, LBT | AS923-1..4, CN470: `NO_RULES`; KR920: `LBT` without LBT, OK with it (10 dBm below 922.1 MHz, 14 dBm from it) |
| `test_txrule` duty cycle | EU868 budgets as before; US915 without a limit; KR920 72 s per channel per hour, one channel apart from another, 17th channel refused; booted in RU864 or AS923-1, then EU868: full budgets; after a frame, a region change starts empty |
| Host, firmware | 23/23 tests (clang, gcc); cppcheck clean; rev3, rev1, desk builds. The fuzz targets do not link on this Mac (Apple's tools have no `libclang_rt.fuzzer_osx.a`): CI builds them |
| Board, EU868 | final desk build, FiPy at 2 dBm: joined in 5 s (the join-accept went through the new check), 2 uplinks, no panic |
| `test_mac` | TxParamSetupReq bytes (0x09, then 0x15 or 0x2D); the MaxEIRP table; TxParamSetupAns parsed |
| `test_ns` `tx_param` | AS923-1 1.0.2: sent on the first uplink (09 15); no answer: again after 4 uplinks; answered: the larger sizes, not sent again; a new join resets it. EU868, US915: never. AU915: 1.0.4 after the pair (09 1D), never beside it; 1.0.2: never |
| `test_ns` mutation | with uplink dwell off in the command, the two byte checks fail |
