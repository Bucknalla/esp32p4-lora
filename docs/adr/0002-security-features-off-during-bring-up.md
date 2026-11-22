# ADR-0002: Secure boot, flash encryption and NVS encryption stay off during bring-up

- Status: accepted (NVS encryption via the HMAC scheme evaluated and deferred: ADR-0006)
- Date: 2026-11-21

## Context

The design plan requires secure boot v2, flash encryption (release mode on
production units) and encrypted NVS. On the ESP32-P4, enabling secure boot or
flash encryption burns eFuses on first boot. This is permanent, also in
"development mode". A wrong key or a wrong mode can lock a board out of
reflashing.

## Decision

- `sdkconfig.defaults` turns off `CONFIG_SECURE_BOOT`,
  `CONFIG_SECURE_FLASH_ENC_ENABLED` and `CONFIG_NVS_ENCRYPTION`.
- The partition table already has the `nvs_keys` partition, so turning on NVS
  encryption later does not change the layout.
- The partition table sits at 0x10000, not 0x8000. A secure-boot bootloader is
  larger, and the table offset cannot change by OTA after units ship.
- No `espefuse burn_*` command runs without a written procedure and explicit
  approval per board.

## Consequences

- Bench boards stay reflashable over UART.
- A later ADR defines the key custody (signing key location, who can sign),
  the development-mode procedure, and the board(s) it is tested on first.

## Baseline

Bench board MAC <bench MAC>, 2026-11-21: no security eFuses burned
(`SECURE_BOOT_EN` false, `SPI_BOOT_CRYPT_CNT` 0, all `KEY_PURPOSE_n` USER).
