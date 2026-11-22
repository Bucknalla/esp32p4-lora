# ADR-0006: NVS encryption with the HMAC scheme (no flash encryption)

- Status: **deferred** (2026-11-21). The owner declined the eFuse burn for
  now: without secure boot the scheme protects only against flash dumps.
  Revisit together with the secure boot / key-custody decision.
- Date: 2026-11-21
- Would amend ADR-0002 (which keeps NVS encryption off).

## Context

Wi-Fi credentials, and later device root keys, must not sit in plain text in
flash. ESP-IDF offers two NVS encryption schemes:

1. Flash-encryption based: keys in the `nvs_keys` partition, protected by flash
   encryption. Needs flash encryption, which ADR-0002 keeps off.
2. HMAC based (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, the P4 has the HMAC
   peripheral): the NVS XTS keys are derived on-chip from a 256-bit HMAC key in
   an eFuse key block. Flash encryption and secure boot stay off.

## Decision (if accepted)

- Scheme 2. `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=5` (eFuse BLOCK_KEY5).
- On the first `nvs_flash_init` with an empty BLOCK_KEY5, the firmware
  generates a random key on-chip and burns it with `esp_efuse_write_key`:
  key block write-disabled and **read-disabled** (software cannot read it,
  only the HMAC peripheral uses it), purpose `HMAC_UP`. Nothing else is burned.
- The key never leaves the chip. A computer can therefore no longer build an
  NVS image for the board: credentials go in through the service console
  (`WIFI_SET`, `tools/provision_wifi.py`), and the board writes them itself.
- An existing plain-text NVS partition is unreadable after the switch and is
  erased on first boot (`init_nvs`).
- The `nvs_keys` partition stays in the table, unused, for scheme 1 later.

## Consequences

- Permanent per board: BLOCK_KEY5 is used for good. Key blocks 0–4 stay free
  for secure boot digests and flash encryption.
- The board stays reflashable over UART; no download or JTAG restriction.
- **Limit:** without secure boot, anyone with USB access can flash their own
  firmware, ask the HMAC peripheral for the same NVS keys, and decrypt NVS.
  This scheme protects a flash dump or a desoldered chip, not a board in an
  attacker's hands. Full protection needs secure boot (key-custody ADR).
- If the eFuse burn fails, `nvs_flash_init` fails and the firmware stops at
  boot (ESP_ERROR_CHECK); the board is still reflashable.

## Kept now regardless

- Credentials go in through the service console (`WIFI_SET`,
  `tools/provision_wifi.py`), never through a computer-built NVS image. This
  already matches the encrypted flow, so turning encryption on later needs only
  the three Kconfig lines and the approved eFuse burn.
- NVS is plain text: the Wi-Fi password is readable from a flash dump.
