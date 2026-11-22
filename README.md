# ESP32-P4 LoRaWAN gateway

Single-site LoRaWAN gateway and network server on a Waveshare
ESP32-P4-Module-DEV-KIT with a recycled Nebra concentrator HAT.
Design: [docs/design-plan.md](docs/design-plan.md). Decisions: [docs/adr](docs/adr).

## Layout

| Path | What |
| --- | --- |
| `core/` | OS-independent C11 library (no ESP-IDF / FreeRTOS headers). Host-tested. |
| `firmware/` | ESP-IDF project: board layer, drivers, tasks. Wraps `core/` as the `lwc_core` component. |
| `tools/` | Build, include check, serial log capture. |
| `c6-firmware/` | ESP-Hosted image for the on-module ESP32-C6 (Wi-Fi). See its README. |
| `docs/` | Design plan, ADRs, bring-up log. |

## Toolchain

ESP-IDF version is pinned in `.esp-idf-version` (ADR-0001).

```sh
mkdir -p ~/esp/v6.1.1 && cd ~/esp/v6.1.1
git clone -b v6.1.1 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32p4,esp32c6
. ~/esp/v6.1.1/esp-idf/export.sh
```

## Build and flash

Check the chip revision first, then use the matching build:

```sh
python -m esptool --port <PORT> chip-id      # "revision v3.x" -> rev3, "v1.x" -> rev1
tools/build.sh rev3                          # build/fw-rev3
tools/build.sh rev1                          # build/fw-rev1
tools/build.sh rev3 -p <PORT> flash monitor
```

No TTY (CI, scripts): `python tools/capture_log.py <PORT> 20` resets the board
and prints the log; add `--no-reset` to watch the running board.

Serial port rule (CH343 auto-reset circuit): open the port with DTR and RTS
both on. macOS raises both on open; any tool that sets them off reaches the
RTS-on/DTR-off state for a moment, which resets the board.

## Wi-Fi

```sh
python tools/provision_wifi.py <PORT> "<SSID>"   # asks for the password, writes NVS
tools/flash_c6_image.sh <PORT>                    # C6 firmware via the P4 (see c6-firmware/)
```

## Firmware updates over the network (ADR-0008)

Gateways pull signed images from this repo's
[latest release](https://github.com/Bucknalla/esp32p4-lora/releases/latest)
every 15 minutes, or at once on the console command `OTA_CHECK`.

One-time setup on the build machine, then a USB flash of a signed build:

```sh
tools/ota_keygen.sh                          # ~/.config/esp32p4-lora/ota_signing_key.pem: back it up
tools/build.sh rev3 -p <PORT> flash          # signed, because the key now exists
```

Each release after that (gateways update within one poll interval):

```sh
tools/ota_release.sh v0.3.0                  # clean, pushed tree; builds rev3 + rev1, signs, publishes
```

The gateway installs only images signed with the key. A new image must pass
its health check within 300 s (IP up, update URL reached, radio started) or
the bootloader boots the previous one, and that build is not tried again. A
reset in that window also rolls back: watch with `capture_log.py --no-reset`.
The console command `STATUS` prints version, slot, state, IP and the last
check result. After OTA updates, use `flash` (not `app-flash`) over USB.

## Host tests

```sh
tools/check_core_includes.sh
cmake -S core -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
```

Fuzzing needs a clang with libFuzzer (not Apple clang); CI runs it.

## Security

Secure boot, flash encryption and NVS encryption are **off** (ADR-0002).
They burn eFuses permanently. Do not turn them on without the key-custody ADR.

## License

Apache-2.0 (see `LICENSE`), except bundled third-party code under its own
license: Semtech `lora_gateway` and `sx1302_hal` (Revised BSD, in
`firmware/components/sx1301_hal` and `sx1302_hal`), ESP-IDF components and
ESP-Hosted (Apache-2.0).

