# Bring-up log

## 2026-11-21 — bench board <bench MAC>, Nebra HAT fitted

**Header pin map: verified against the Waveshare schematic**

Waveshare numbers the header odd/even swapped (its pin 1 = 5 V = Pi pin 2),
but the physical positions match a Pi. Every row of the plan's pin map is
correct:

| Pi pin | Function | P4 GPIO | Waveshare pin |
| --- | --- | --- | --- |
| 3 / 5 | I²C SDA / SCL | 7 / 8 | 4 / 6 |
| 7 | Light II reset | 23 | 8 |
| 15 | Hotspot HAT reset | 6 | 16 |
| 16 | Hotspot HAT button | 5 | 15 |
| 18 | Hotspot HAT LED | 4 | 17 |
| 32 | Light II button | 54 | 31 |
| 35 | SPI MISO | 53 | 36 |
| 36 | SPI CE2 | 46 | 35 |
| 37 | Light II LED | 47 | 38 |
| 38 | SPI MOSI | 27 | 37 |
| 40 | SPI SCLK | 45 | 39 |

GPIO53 also goes through R52 (0 Ω) to the NS4150B amplifier `CTRL`, which has
**R58 10 kΩ to ground**. MISO therefore idles low, and every high bit on MISO
switches the speaker amplifier on. Removing R52 (as the plan says) removes both
effects.

**I²C:** ACK at 0x18 (ES8311 codec on the board) and 0x60 (consistent with the
HAT's ECC608).

**Pin survey** (input only, internal pull-up then pull-down, 20 ms settle):

| GPIO | Role | Result |
| --- | --- | --- |
| 6 | Hotspot HAT reset | held LOW (resistive: weakest drive pulls it high) |
| 23 | Light II reset | floating |
| 4, 47 | LEDs | floating |
| 5 | Hotspot HAT button | floating |
| 54 | Light II button | held HIGH |
| 53 | MISO | held LOW (R58) |

**Concentrator SPI probe** (mode 0, 500 kHz and 2 MHz):

- SX1302 frame `00 56 06 00 00` (version register 0x5606) → `00 00 00 00 00`.
- SX1301 frame `01 00` (version register 0x01, expect 0x67) → `00 00`.
- Same result after a reset pulse on GPIO6 (high 100 ms, low, 100 ms settle;
  Nebra's `reset_lgw.sh` sequence).
- Conclusion: the concentrator does not drive MISO. Open causes: no power to
  the mPCIe card, card not seated or faulty, or the HAT routes SPI differently
  from the Nebra `nebra-light1` definition.

### Update: root cause — wrong SPI pins for this HAT

The HAT is the **Nebra Helium Hotspot HAT V1** (RAK833 backplane,
`NebraLtd/nebra-lora-hardware/Helium-Hotspot-HAT-V1`). Netlist from its Eagle
`.sch`:

| Net | Pi pin | P4 GPIO | Waveshare pin |
| --- | --- | --- | --- |
| SPI_MOSI | 19 (BCM10) | 3 | 20 |
| SPI_MISO | 21 (BCM9) | 2 | 22 |
| SPI_CLK | 23 (BCM11) | 0 | 24 |
| SPI_CS (CE0) | 24 (BCM8) | 36 | 23 |
| LORA_RS1 (reset) | 15 (BCM22) | 6 | 16 |
| BUTTON | 16 (BCM23) | 5 | 15 |
| STATUS LED | 18 (BCM24) | 4 | 17 |
| I2C SDA / SCL (ECC) | 3 / 5 | 7 / 8 | 4 / 6 |
| UART TX / RX → card | 8 / 10 | 37 / 38 | 7 / 9 |
| JP1 FR433 / FR470 / FR868 / FR915 | 32 / 29 / 31 / 33 | 54 / 33 / 26 / 48 | 31 / 30 / 32 / 34 |

Card power: LM1117 on the HAT, from 5 V (Pi pins 2/4).

The plan's SPI1 CE2 map (Pi 35/36/38/40) comes from hm-pyhelper's
`nebra-light1` (a different carrier). Those pins do not connect on HAT V1, so
MISO only saw R58.

**Result with the HAT V1 pin map (2 MHz, mode 0):**

- Card: **MaxIIoT GL5712** (SX1301 + 2× SX1257).
- SX1301 version register 0x01 = **0x67**. The card answers with no reset pulse.
- GPIO6 low = run. The GL5712 datasheet calls mPCIe pin 22 "active-low reset";
  on this HAT the pin behaves as the SX1301 active-high reset, as in Nebra's
  `reset_lgw.sh` (high 100 ms → low).
- JP1: **FR868 jumper fitted** (GPIO26 held low). GPIO33 and GPIO54 read high
  with both pulls; the cause is not known.

**P4 pin notes for this map** (source given for each):

- GPIO2–5 are the pad-JTAG pins MTCK/MTDI/MTMS/MTDO (IDF
  `soc/esp32p4/.../io_mux_reg.h`). Used as GPIO, pad JTAG is lost; USB-JTAG
  (GPIO24/25) is a separate path.
- GPIO0/1 (32 kHz crystal) and GPIO36 (strapping): **not verified** in this
  session (chip datasheet, not in IDF headers). The board boots normally with
  the HAT fitted.
- Waveshare carrier: GPIO0, 1, 2, 3, 26, 36 and 54 go only to the header (each
  net appears only on the header and module pin lists). GPIO53 also has R52/R58.
- GPIO54 reads high with both pulls, and nothing on the carrier drives it, so
  the source is inside the module. ESP-Hosted's default C6 reset GPIO for P4 +
  C6 boards is 54 (`Kconfig.host.spi_hd`), so this is **probably the C6
  reset**. Do not fit the JP1 FR433 jumper: it would hold the C6 in reset.
- HAT ID EEPROM I2C (Pi 27/28, 4.7 kΩ pull-ups to 3.3 V) lands on Waveshare
  pins 28/27 = USB1P1_N/P = GPIO24/25, the P4 USB-Serial-JTAG data lines. The
  console uses the CH343, so bring-up is not affected, but USB on that port
  will not work with the HAT fitted.
- GPIO37/38 (Pi 8/10) are the P4 console UART and go to mPCIe pads 31/33. The
  GL5712 leaves them unconnected. Check any other card before fitting
  (ADR-0004).

### Update: SX1301 HAL runs on the GL5712 (ADR-0005)

```
lgw_connect: no FPGA detected
SX125x #0 version 0x21, clock output disabled, PLL start (attempt 1)
SX125x #1 version 0x90, clock output enabled,  PLL start (attempt 1)
calibration finished (status = 159)   -> 0x9F
lgw_start OK in 3081 ms (Version: 5.0.1;)
listen 60 s on EU868 (RX only): 0 packets
lgw_stop OK; HAL task stack: 15516 of 24576 bytes free
```

- 0 packets is not a failure: the antenna and the card suffix (-EX/-UX) are
  not confirmed, and nearby EU868 traffic is not known.
- SX125x #1 reads version 0x90 (expected 0x21). Calibration reaches radio B.
  Open point: compare with a second card.

### Update: Wi-Fi through the ESP32-C6 (ESP-Hosted over SDIO)

- ESP-Hosted 3.1.0 + esp_wifi_remote 1.6.5, defaults for P4 + C6 boards: SDIO
  slot 1, 4-bit, 40 MHz, CLK 18 / CMD 19 / D0–D3 14–17, C6 reset GPIO54. These
  pins are not on the Waveshare carrier, so the SDIO link is inside the module.
  This confirms GPIO54 is the C6 reset.
- Link up: "slave chip id 0x0d (esp32c6)", scan found 10 networks.
- **C6 firmware reports ESP-Hosted 0.0.0** (factory image) against host 3.1.0:
  "major version mismatch — OTA coprocessor from host". It runs in a
  compatibility mode. Updating the C6 is a separate, approved step.
- Credentials live only in NVS (`tools/provision_wifi.py`), never in the repo
  or the image. NVS encryption is off (ADR-0002), so they are plain text in
  flash.

### Update: C6 updated to ESP-Hosted 3.1.0 through the P4

- Factory C6 firmware reported 0.0.0 and does not answer the version RPC; its
  version comes from the init event. RPCs work only after
  `EH_HOST_EVENT_CP_INIT`.
- First attempts failed: (1) the check ran before the init event, (2) 1500-byte
  chunks exceed `EH_HOST_CP_OTA_CHUNK_MAX` (SDIO 1536 B buffer minus headers),
  so `ota_write` failed at once. Fixed: wait for CP_INIT, 1 KB chunks.
- Third attempt: 1,163,056 bytes in ~11 s, restart, then
  `host=3.1.0 coprocessor=3.1.0 (match)`, RPC V2, SDIO SW_AGGR. Scan OK.
- `cp_fw` partition (2 MB at 0xE00000) added; `storage` is now 5.75 MB.

### Update: Semtech UDP packet forwarder to TTN

- Gateway EUI `<gateway EUI>` (base MAC <bench MAC>, FFFE inserted).
- Radio task (core 1): lgw_start OK in 3.1 s, continuous receive, queue of 16,
  stack 9 KB used of 16 KB.
- Forwarder (core 0): DNS eu1.cloud.thethings.network → 52.212.223.226 /
  63.34.215.128, PULL_DATA every 10 s, stat every 30 s.
- First run (gateway not yet registered on TTN): no PULL_ACK or PUSH_ACK.
  TTN drops traffic from unknown EUIs without a reply; a blocked outbound UDP
  port on the IoT network would look the same. The forwarder reconnects
  after 60 s without PULL_ACK, as designed.
- After registering `<gateway EUI>` on TTN eu1 (Europe 863-870, SF9 RX2):
  the socket opened before registration still got no reply; after the
  60 s reconnect, PULL_ACK came at once. On a fresh boot: PULL_ACK 0.1 s after
  connect, every `stat` PUSH_DATA acknowledged (ackr 100 %).
- First RF seen: 867.9 MHz SF7 BW125, RSSI -119 dBm, SNR -12.5 dB, 88 B,
  CRC bad (not forwarded). The receive chain hears 868 MHz band traffic.

### Update: serial resets fixed, downlinks enabled (ADR-0007)

- Serial: macOS raises DTR and RTS on open; pyserial then applied DTR before
  RTS, giving a moment of RTS-on/DTR-off, which pulls EN low. Opening with
  both on (no change) does not reset the board, on open or on close. Tools now
  do that; `capture_log.py` resets only on purpose (`--no-reset` to watch).
- TX: see ADR-0007 for the test frame trace and limits.

### Update: OTA signing and rollback (ADR-0008), first as push

- One USB flash brought the rollback bootloader (0x5B30 bytes, 59 % free)
  and the first signed image (RSA-3072 signature block, 1 MB app).
- Boot: `reset reason power-on; running ota_0 (valid)`; OTA server on port
  8032 with no token: requests refused until `OTA_TOKEN` was set.
- Upload over Wi-Fi: ~70 KB/s (1 MB in 15 s). During one upload the radio
  received a packet, a `stat` was acked by TTN (1/1) and radio restarts
  stayed 0.
- All tests in ADR-0008 passed. A reset from `capture_log.py` (EN pin) while
  an image is pending verify rolls it back, so watch a new image with
  `--no-reset`.

### Update: OTA changed to esp_https_ota pull from GitHub releases

- First host: public files-only repo `Bucknalla/esp32p4-lora-firmware`
  (source then private); the gateway polls
  `releases/latest/download/lorawan_gateway-rev3.bin`. Later the source repo
  went public and became the host (see below). GitHub answers
  with two redirects (to `release-assets.githubusercontent.com`);
  esp_https_ota follows them over TLS with the IDF CA bundle.
- The pull build went onto the board through the old push endpoint one last
  time; since then, updates come only from releases.
- Header check ~3.5 s; full 1.2 MB download and flash ~18 s.
- All pull tests in ADR-0008 passed. Test releases were deleted afterwards;
  v0.2.0-dev.1 and -dev.2 (dirty-tree builds) remain, dev.2 is latest and
  runs on the board.

### Update: supervisor, fault record, retained log ring (ADR-0009)

- Dev build flashed over USB; auto-install off, so it reported
  "update available" for v0.2.0 instead of replacing itself.
- `TEST_ABORT`: coredump in flash (16 KB erased, ~16 KB written), fault note
  kept in RTC memory, panic counted. First try of the PSRAM ring kept nothing:
  header magic and size (first 8 bytes at 0x48000000) were overwritten during
  boot while head, used and CRC survived. With a 4 KB guard the ring kept
  4.9 KB and printed the previous boot's last lines.
- `TEST_STALL`: supervisor abort after 30 s naming `console`; `idf.py
  coredump-info` shows `sv` in `esp_system_abort` and the console task in
  `handle_line`.
- Outage build (DNS name that cannot resolve, unroutable OTA URL, 1 min
  poll) ran 12 min with no abort.
- 31 min soak of the dev build: no abort or reset, every TTN stat
  acknowledged, three update checks (each "update available", not
  installed), least free stack 2.7 KB (main task).
