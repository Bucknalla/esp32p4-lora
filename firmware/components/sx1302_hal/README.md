# sx1302_hal (Semtech libloragw) for ESP-IDF

## Provenance

- Upstream: https://github.com/Lora-net/sx1302_hal
- Pinned: V2.1.0, commit `4b42025d1751e04632c0b04160e0d29dbbb222a5`
- License: Revised BSD (`LICENSE.TXT`). `libtools/tinymt32.*` is BSD-3-Clause
  (Saito, Matsumoto; header in the file).
- `libloragw/` and `libtools/` are generated. Do not edit them by hand. Change
  `tools/import_sx1302_hal.py` and re-run it:

  ```sh
  git clone https://github.com/Lora-net/sx1302_hal.git /tmp/sx1302_hal
  python3 tools/import_sx1302_hal.py /tmp/sx1302_hal
  ```

## Patches to upstream (`patches/esp32p4-port.diff`)

| File | Change | Why |
| --- | --- | --- |
| `loragw_spi.c`, `sx1250_spi.c`, `sx125x_spi.c`, `sx1261_spi.c` | `open`/`close`/`ioctl` on the spidev fd call `lgw_port_spi_*`; `<linux/spi/spidev.h>` replaced by `lgw_port_spidev.h` | No Linux spidev. Frame building stays upstream. |
| `loragw_aux.c` | `wait_us`/`wait_ms` call `lgw_port_wait_*` | No `clock_nanosleep`; delays must never be short. |
| `loragw_hal.c` | Temperature sensor optional, fixed value fallback (3 places) | Nebra carriers do not route the card's I²C, so `lgw_start` failed with "no temperature sensor found". |
| `loragw_hal.c` | No `symlink`/`unlink` for the debug log | Not available on the target. |

## Not built

- `loragw_usb.c`, `sx1250_usb.c`, `sx1261_usb.c`, `loragw_mcu.c`: USB
  concentrators. Stubs in `port/lgw_port_misc.c` return errors.
- `loragw_gps.c`: Class B is out of scope.
- `loragw_i2c.c`: Linux i2c-dev. Stubs return errors (no sensor is reachable).

## Platform layer (`port/`)

- `lgw_port_spi.c`: the only owner of the SPI bus. No DMA: polled transfers of
  `SOC_SPI_MAXIMUM_BUFFER_SIZE` (64) bytes, chained with
  `SPI_TRANS_CS_KEEP_ACTIVE`, so CS stays low for a whole `SPI_IOC_MESSAGE(n)`.
  On the P4, DMA RX needs a cache-aligned address and length, or `spi_master`
  allocates a bounce buffer per transfer, which the engineering rules forbid
  on the radio path.
- `lgw_port_misc.c`: delays (busy-wait below one tick, otherwise round up plus
  one tick), the fixed temperature, and the stubs.
- `lgw_port.h` uses only standard C types, so the patched upstream files do not
  depend on ESP-IDF. A Zephyr port replaces `port/` only.

## Build flags

Upstream sources build with `-Wno-error`, and with `-Wformat` and
`-Wunterminated-string-initialization` off. On this RISC-V ABI `uint32_t` is
`long`, so upstream's `%u` produces only format warnings. The 4-byte tag strings
are intentional. Port files build with `-Wall -Wextra -Werror` (ADR-0004).

## Known upstream issues and follow-ups

- `loragw_hal.c` duplicate-packet removal uses `abs()` on an unsigned
  `count_us` difference, so it only matches duplicates in one order
  (`-Wabsolute-value`). Fix it when `lgw_receive` is in use.
- `timeout_start`/`timeout_check` and `_meas_time_*` in `loragw_aux.c` use
  `gettimeofday` (wall clock). An SNTP step can then break HAL timeouts.
  Move them to a monotonic clock before SNTP is enabled.
- `lgw_start` probes I²C for an STTS751 before the patched fallback. The
  port's `i2c_linuxdev_open` succeeds with a dummy handle and every transfer
  fails, so the probe reports "no sensor" instead of aborting.

## Verified on hardware

- 2026-11-21, GL5712 (SX1301) card on the Helium HAT V1: port SPI single and
  two-segment (CS held) reads return the SX1301 version 0x67 at 2 MHz.
- Not yet proven: messages longer than one 64-byte chunk (the firmware load
  writes and verifies 8 KB), and `lgw_start` on an SX1302/SX1303 card.
