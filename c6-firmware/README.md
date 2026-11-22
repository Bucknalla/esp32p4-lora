# ESP32-C6 co-processor firmware

ESP-Hosted co-processor image for the C6 on the Waveshare module (Wi-Fi for the
P4 over SDIO). Source: ESP-Hosted 3.1.0 `examples/ota/coprocessor_ota/cp`,
pinned to `espressif/esp_hosted ==3.1.0` so it matches the host side.

- Transport: SDIO slave (C6 fixed pins 18–23), SW aggregation, RPC V2.
- Bluetooth is off (the factory image had HCI over SDIO; the plan has no BLE).
- `PROJECT_VER` = the ESP-Hosted version, so logs show it.

## Update path (no C6 cable needed)

```sh
. ~/esp/v6.1.1/esp-idf/export.sh
tools/flash_c6_image.sh <port>      # build, write to the P4's cp_fw partition
```

On the next boot `firmware/main/c6_update.c` compares the C6's ESP-Hosted
version (from its init event) with the host's. If major.minor differ, it pushes
`cp_fw` with the OTA RPCs in 1 KB chunks and restarts. An NVS counter
(`c6upd/tries`, max 3) stops a reboot loop; it resets when the versions match.

## Recovery

If the C6 does not come back, header **P2** on the carrier has C6 TXD, RXD,
GND and `C6_IO9` (boot strap). Hold IO9 low at reset and flash with esptool
over a 3.3 V USB-UART adapter.
