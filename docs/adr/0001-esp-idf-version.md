# ADR-0001: Pin ESP-IDF v6.1.1

- Status: accepted
- Date: 2026-11-21

## Context

The design plan asks for one pinned ESP-IDF release that supports our chip
revision. Candidates on 2026-11-21: v5.5.6 (latest 5.5.x) and v6.1.1 (latest
stable).

Both releases support ESP32-P4 v3.x silicon and v1.x through
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, so chip support does not decide it. The
deciding factor is the managed components the plan needs:

| Component | Latest | IDF range | Notes |
| --- | --- | --- | --- |
| `espressif/esp-cryptoauthlib` | 3.7.9~2 (2026-07-30) | >=4.3 | Added a PSA Crypto port for the IDF 6.x mbedTLS (commit "Add PSA Crypto integration for ATECC608A") |
| `espressif/esp_hosted` | 3.1.0 (2026-11-05) | >=5.5 | Lists `esp32p4` |
| `espressif/esp_wifi_remote` | 1.6.5 | >=5.3 | |
| `espressif/esp_modem` | 2.1.0 | >=4.1 | |
| `joltwallet/littlefs` | 1.23.1 | >=5.0 | |
| `espressif/ip101` | 1.1.0 | >=6.0 | IP101 driver left ESP-IDF in 6.0 |

Manifest ranges are open lower bounds, so they were also compiled: a scratch
project that depends on `esp-cryptoauthlib`, `esp_hosted`, `esp_wifi_remote`,
`esp_modem` and `joltwallet/littlefs` (versions above) builds for `esp32p4` on
v6.1.1 with no errors (2026-11-21). This is compile level only. It answers the
plan's open question "does `esp-cryptoauthlib` support the P4 as-is" for the
build; runtime on the ECC608 is still a phase 4 check.

## Decision

Use ESP-IDF **v6.1.1**.

- Recorded in `/.esp-idf-version`, `firmware/main/idf_component.yml`
  (`idf: "==6.1.1"`) and checked at configure time in `firmware/CMakeLists.txt`.
- `firmware/dependencies.lock` is committed.
- Two builds: `rev3` (v3.x silicon, min rev v3.1) and `rev1`
  (`ESP32P4_SELECTS_REV_LESS_V3=y`, max rev v1.99). `tools/build.sh` selects one.

## Consequences

- Longer support window than 5.5.x.
- Secure-element work uses the PSA path of `esp-cryptoauthlib`, which is new
  (July 2026). Phase 4 must verify it on the ECC608 before relying on it.
- Moving to a new IDF release is a deliberate change with its own ADR.

## Evidence

- Bench board: ESP32-P4 rev v3.2, 16 MB flash (GigaDevice), 32 MB AP PSRAM at
  200 MHz. Bring-up image from this repo boots on v6.1.1 (2026-11-21).
