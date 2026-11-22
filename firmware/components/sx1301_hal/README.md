# sx1301_hal (Semtech lora_gateway libloragw) for ESP-IDF

## Provenance

- Upstream: https://github.com/Lora-net/lora_gateway (no longer maintained)
- Pinned: v5.0.1, commit `a955619271b5d0a46d32e08150acfbc1eed183b7`
- License: Revised BSD (`LICENSE`).
- `libloragw/` is generated. Change `tools/import_sx1301_hal.py` and re-run it:

  ```sh
  git clone https://github.com/Lora-net/lora_gateway.git /tmp/lora_gateway
  python3 tools/import_sx1301_hal.py /tmp/lora_gateway
  ```

## Patches (`patches/esp32p4-port.diff`)

| File | Change | Why |
| --- | --- | --- |
| `loragw_spi.native.c` | `open`/`close`/`ioctl` call `lgw_port_spi_*`; spidev header replaced | No Linux spidev. Frame building stays upstream. |
| `loragw_spi.native.c` | `SPI_SPEED` 8 MHz → 2 MHz | Nebra builds at 2 MHz. The bus clock itself comes from Kconfig. |
| `loragw_aux.c` | `wait_ms` calls `lgw_port_wait_ms` | No `clock_nanosleep`; delays must never be short. |

Not built: `loragw_gps.c` (Class B is out of scope).

`config/config.h` replaces the Makefile's generated header. `DEBUG_HAL` and
`DEBUG_REG` follow `CONFIG_GW_LGW_DEBUG`.

The platform layer is the shared `lgw_port` component.

## Verified on hardware

See ADR-0005: `lgw_start` (firmware load, calibration 0x9F), RX loop and
`lgw_stop` on a GL5712, 2026-11-21.
