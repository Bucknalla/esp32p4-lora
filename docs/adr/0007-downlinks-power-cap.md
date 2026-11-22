# ADR-0007: Downlinks on the SX1301, capped at 13 dBm

- Status: accepted
- Date: 2026-11-22

## Context

TTN needs downlinks for OTAA joins, confirmed uplinks and MAC commands. The
GL5712 can transmit up to +27 dBm (700 mA peak). The board is powered from the
Mac's USB port, the HAT feeds the card from an LM1117, and the antenna is the
Nebra stock one (~3 dBi). EU868 allows 14 dBm ERP on most sub-bands
(16.15 dBm EIRP), and 27 dBm ERP on 869.4–869.65 MHz.

## Decision

- TX on RF chain 0 (Nebra EU868: `radio_0` `tx_enable`, 863–870 MHz).
- Power cap `CONFIG_GW_TX_MAX_DBM` = 13 dBm at the connector, applied by
  cutting Nebra's TX gain LUT, so no request can exceed it. It applies to RX2
  (869.525 MHz) too until supply current is measured; TTN gets
  `txpk_ack` `warn: TX_POWER` when a request is capped.
- Timing: live concentrator counter with `GPS_EN=0` (no GPS); core JIT queue
  admits frames 20 ms–10 s ahead, hands them to `lgw_send` 40 ms before start,
  checks `TX_FREE`, aborts a frame stuck after its end.
- Airtime from `lwc_lora_airtime_us` (core), not `lgw_time_on_air`: the HAL's
  function uses the request's preamble field (0 = default) while `lgw_send`
  sends 8 symbols, so it under-counts by 8 symbols.
- Duty cycle enforced per ETSI sub-band in the gateway (token bucket, 1 h).
- After each frame the counter must advance with the host clock; otherwise the
  radio task restarts the HAL (card reset, e.g. a supply dip).

## Evidence (2026-11-22, bench board, GL5712)

- Calibration status 0xBF with TX on (radio A TX DC offset calibrated).
- Counter check: +100 487 µs over 100 499 µs host time.
- One approved test frame: 869.525 MHz, SF9, −6 dBm, inverted polarity,
  proprietary MHDR. `lgw_send` took 2.2 ms, handed over 38 ms before start;
  EMITTING from −0.97 ms to +145 ms (computed 144.4 ms); counter continuous;
  no brownout or reset on USB power.

## Not yet proven

- Real output power and frequency (no RF instrument).
- A TTN downlink end to end (needs a device's uplink first).
- Behaviour above −6 dBm on USB power: raise the cap only after measuring
  the supply.
