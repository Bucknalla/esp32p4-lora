# ESP32-P4 LoRaWAN Gateway — Design Plan

Nov 20, 2026 · @Alex Bucknall

## Overview and goals

A Waveshare ESP32-P4-Module-DEV-KIT with a recycled Nebra concentrator HAT replaces a Raspberry Pi gateway, running the whole single-site LoRaWAN stack on the microcontroller: packet forwarder, network server, join server and application layer.

**Goals**

- Standards-compliant 8-channel EU868 gateway on an SX1302/SX1303 concentrator.
- Self-contained network for our own devices: OTAA joins, Class A, ADR, MQTT output. Keeps working with no internet.
- Hybrid mode: packets from unknown devices forwarded upstream to TTN or a central ChirpStack.
- Pi parity for operations: signed OTA updates, web management, remote logs, automatic recovery.
- Powered and networked over one Ethernet cable via PoE.

**Non-goals for v1**

- Multi-gateway deduplication or fleet management.
- Class B (needs GPS PPS and beaconing).
- FUOTA, LoRaWAN 1.1 split keys, relay.
- Long-term data storage and dashboards. These live in whatever consumes the MQTT feed.

**Why the P4 over a Pi:** no SD card to corrupt, boots in about a second, no OS to patch, secure boot and flash encryption on chip, and a lower bill of materials.

## Platform: ESP-IDF first

We build on ESP-IDF now and keep the door open to Zephyr: ESP-IDF supports everything this design needs on the P4 today, including the security features the gateway can't ship without.

**Why ESP-IDF now**

- Secure boot v2 and flash encryption are production-ready on the P4. Under Zephyr they come through MCUboot's Espressif port, which lists P4 support as in progress.
- FreeRTOS SMP runs both HP cores, so the core 1 / core 0 split in the architecture works as drawn. Zephyr's P4 SMP support is still in open pull requests ([#120181](https://github.com/zephyrproject-rtos/zephyr/pull/120181), [#126531](https://github.com/zephyrproject-rtos/zephyr/pull/126531)).
- Secure element support (`esp-cryptoauthlib` with `esp-tls`), `esp-modem`, ESP-Hosted and the Ethernet driver are all first-party.

**Version pinning**

- One pinned ESP-IDF release that supports our chip revision, recorded in the repo.
- v1.3 and v3.x silicon need separate builds (`ESP32P4_SELECTS_REV_LESS_V3`); check each board with `esptool.py chip_id` before flashing.

**Keeping Zephyr open**

- Network server, join server, ADR, persistence logic and the hybrid-forwarding decision live in an OS-independent C library — no FreeRTOS or ESP-IDF calls — unit-tested on a PC.
- A thin platform layer supplies SPI, GPIO, timers, storage, crypto and networking. Only this layer changes in a move.
- Revisit Zephyr once MCUboot's P4 secure boot and flash encryption and P4 SMP are merged upstream.

## Engineering principles

These rules apply to every phase and are enforced by CI and code review rather than left to good intentions.

**Memory**

- No heap allocation on the radio fast path after start-up; buffers come from fixed-size pools allocated at init.
- Placement is explicit: DMA buffers and anything an ISR touches in internal SRAM (`MALLOC_CAP_DMA` / `MALLOC_CAP_INTERNAL`); bulk tables and queues in PSRAM.
- Stack sizes set per task and checked against high-water marks in tests; free heap per region published in metrics.

**Concurrency and timing**

- Each resource has one owning task; other tasks reach it through queues, not shared variables.
- ISRs do the minimum (clear, capture, notify) and defer the rest to a task — no blocking, logging or floating point in ISRs.
- Task priorities are documented in one table, highest for the radio path. Locking uses mutexes (priority inheritance), never binary semaphores.
- Every blocking call has a timeout; no unbounded busy-waits.
- Uplink and downlink timing budgets are measured on hardware, not assumed.

**Errors and recovery**

- Every `esp_err_t` is checked; failures are logged with context and counted in metrics.
- Queues are bounded, with an explicit drop policy and a drop counter.
- Concentrator, network links and MQTT each have a small recovery state machine: retry with backoff → reset the peripheral → reboot as a last resort.
- Reset reason and last fault are persisted and reported on the next boot.
- Brownout detection on; the task watchdog covers every long-lived task.

**Persistence**

- Writes are power-loss safe: versioned records with a CRC, written to a new slot then switched over, never edited in place.
- Every record format carries a schema version, and migration code is tested on upgrade.

**Untrusted input**

- Radio frames, MQTT messages, REST requests and config uploads are untrusted: length-checked parsers, no unbounded copies.
- Parsers are fuzzed on the host (libFuzzer) in CI.
- Secrets never appear in logs, API responses or coredumps.

**Structure and portability**

- The core library includes no ESP-IDF or FreeRTOS headers; a CI check enforces it.
- Board specifics (pins, HAT variant, chip revision) live in one board config, selected through Kconfig.
- Build-time options in Kconfig; runtime settings in NVS, validated, with safe defaults.

**Code quality and testing**

- C11, warnings as errors (`-Wall -Wextra -Werror`), clang-format for style, BARR-C as the coding standard reference.
- Static analysis (clang-tidy, cppcheck) on every commit.
- Host unit tests for the core library run under ASan and UBSan.
- Hardware-in-the-loop tests on a bench gateway with test end devices back every phase gate.
- CI builds both chip revisions on every commit.
- Significant design decisions are recorded as short ADRs in the repo; pull requests are reviewed against this section.

## Hardware

The Nebra HAT plugs onto the Waveshare 40-pin header with no adapter: the header is electrically a Raspberry Pi header, and every signal the HAT uses lands on a free P4 GPIO.

**Bill of materials**

| Part | Role | Notes |
| --- | --- | --- |
| [Waveshare ESP32-P4-Module-DEV-KIT](https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT) | Host | ESP32-P4NRW32X (v3.x silicon; older stock may be v1.3), 32 MB PSRAM, ESP32-C6 for Wi-Fi 6/BLE over SDIO, 16 MB flash, IP101 Ethernet PHY, Pi form factor |
| Nebra Hotspot HAT | Carrier | From retired Helium miners; ECC608 secure element on I²C at 0x60 |
| Nebra mPCIe concentrator | Radio | SX1302/SX1303 only; must be the 868 MHz variant. SX1301 cards are spares |
| Waveshare PoE module | Power | Optional; plugs into the board's PoE header |
| EU868 antenna + pigtail | RF | Matched to the concentrator's u.FL connector |

**Pin map** (original Hotspot HAT, from Nebra's [hardware definitions](https://github.com/NebraLtd/hm-pyhelper/blob/master/hm_pyhelper/hardware_definitions.py) and the [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-P4-Module-DEV-KIT/ESP32-P4-Module-DEV-KIT.pdf))

| Function | Pi pin | BCM | P4 GPIO | Shared with |
| --- | --- | --- | --- | --- |
| SPI SCLK | 40 | 21 | GPIO45 | microSD power switch |
| SPI MOSI | 38 | 20 | GPIO27 | — |
| SPI MISO | 35 | 19 | GPIO53 | Speaker amp enable (via R52) |
| SPI CS (CE2) | 36 | 16 | GPIO46 | — |
| Concentrator reset | 15 | 22 | GPIO6 | — |
| Status LED | 18 | 24 | GPIO4 | — |
| Button | 16 | 23 | GPIO5 | — |
| I²C SDA / SCL | 3 / 5 | 2 / 3 | GPIO7 / GPIO8 | Audio codec, camera, display |

Light Hotspot II HAT variant: reset on GPIO23 (Pi 7), LED on GPIO47 (Pi 37), button on GPIO54 (Pi 32). SPI is the same.

**Conflicts and how we handle them**

- GPIO45 toggles microSD power during SPI traffic → the SD slot is not used in this design.
- GPIO53 toggles the speaker amplifier → remove R52 (0 Ω) on production units.
- Waveshare numbers the header rows opposite to a Pi → before first fit, confirm 5 V at physical Pi pin 2/4 with a meter.

**Power:** 5 V from USB-C or the PoE module. The concentrator's peak draw during transmit must fit the PoE module's rating — to be measured (see Risks).

## System architecture

The P4's two cores split the work so nothing networked can delay a radio deadline: core 1 runs only the radio path and the network server, core 0 runs everything that talks to the outside world.

&#91;embedded content: firmware architecture · two cores, shared storage\]

Uplinks flow down core 1 to the network server, which passes known devices to the application layer and everything else to the hybrid forwarder. Downlinks from both the local network server and the upstream server go through the one scheduler, so the duty-cycle budget is enforced in a single place.

**Tasks and communication:** FreeRTOS SMP with tasks pinned by `xTaskCreatePinnedToCore` — radio task, network server and downlink scheduler on core 1, everything else on core 0. PSRAM-backed FreeRTOS queues connect them; the only state shared across cores is the device table, guarded by a single lock.

**Flash writes pause the other core:** while ESP-IDF erases or writes flash it disables the cache and holds the other core in a wait loop; only interrupt handlers placed in internal RAM keep running, and PSRAM is unreachable for the duration. The concentrator buffers packets in hardware, so short pauses are harmless. The radio path's SPI interrupt handler and buffers live in internal SRAM, and flash writes stay short and batched (see persistence rules).

## Memory and flash layout

RAM is not the constraint — about 1 KB per device puts 1,000 devices near 1 MB of the 32 MB PSRAM — so the design work goes into persistence that doesn't wear out flash.

**RAM budget**

| Region | Holds |
| --- | --- |
| Internal SRAM (768 KB) | Task stacks, HAL and SPI buffers, lwIP, the uplink/downlink fast path |
| PSRAM (32 MB) | Device and session tables, packet queues, log ring buffer, MQTT/HTTP buffers, codec engine heap |

**Partition table (16 MB flash)**

| Partition | Type | Size | Contents |
| --- | --- | --- | --- |
| nvs | data/nvs (encrypted) | 64 KB | Config, session keys, root keys, counters |
| nvs\_keys | data/nvs\_keys | 4 KB | NVS encryption keys |
| otadata | data/ota | 8 KB | Active slot + rollback state |
| ota\_0 | app | 4 MB | Firmware slot A |
| ota\_1 | app | 4 MB | Firmware slot B |
| coredump | data/coredump | 64 KB | Last crash for remote debugging |
| storage | data/littlefs | Remainder (\~7 MB) | Device database, web UI assets, persisted logs |

**Persistence rules**

- Frame counters live in RAM. Each device's counters are written to NVS every N frames or every few minutes, whichever comes first, batched across devices.
- On boot, every downlink counter jumps ahead by N, so no value is ever reused after a crash.
- DevNonce history and session keys are written on each join (rare event, no wear concern).
- Device records are stored as one CBOR file per device in LittleFS, rewritten only when provisioning or settings change.
- Optional later: a small FRAM on I²C for write-every-frame counters with no wear.

## Radio and packet forwarder

We port Semtech's `sx1302_hal` to ESP-IDF and keep its logic intact; only the platform layer underneath it changes.

**Porting layer**

| Linux dependency in the HAL | ESP-IDF replacement |
| --- | --- |
| `spidev` read/write | `spi_master` on GPIO45/27/53/46, 2 MHz default |
| `reset_lgw.sh` | GPIO6 driven high \~100 ms, then low, \~100 ms settle |
| `pthread`, `usleep` | FreeRTOS tasks and `vTaskDelay` / `esp_rom_delay_us` |
| `global_conf.json` on disk | Config struct from NVS, editable in the web UI |
| Firmware blobs for the SX1302 cores | Compiled into the image as const arrays |

**Timing:** downlinks don't need tight timing from the host. The concentrator transmits on its own internal counter; the host only has to queue each packet early enough. A just-in-time queue (as in Semtech's packet forwarder) sends each one to the concentrator shortly before its slot.

**Known quirk:** concentrators pulled from Helium miners sometimes fail the HAL's temperature-sensor check on start-up. Make that check optional in config.

**First output: Semtech UDP protocol** to TTN or ChirpStack. It has no TLS but is the fastest way to prove the radio path end to end. In later phases the same receive path feeds the local network server, with UDP kept for hybrid forwarding.

**TTN link: LoRa Basics Station** (ADR-0016, next after phase 3). The LNS protocol over a TLS WebSocket, authenticated with a TTN API key that can only connect this gateway, replaces UDP as the recommended way to reach TTN: encrypted, downlinks confirmed, and the TTN console shows the gateway as connected. UDP stays for ChirpStack and other servers that use it.

## Network server and join server

With one gateway there is no deduplication, so the network server reduces to per-device state, MAC handling and downlink scheduling — all light work for the P4's AES hardware. ChirpStack's open-source code is the behavioural reference for MAC commands and ADR.

**v1 scope**

- LoRaWAN 1.0.4, EU868 regional parameters.
- Class A (v1), Class C (later phase). Class B out of scope.
- OTAA joins; ABP supported for test devices only.

**Uplink path**

1. Look up the device by DevAddr. Unknown DevAddr → hand to hybrid forwarder.
2. Verify the MIC (AES-CMAC) and the frame counter (must be greater than the last seen).
3. Process MAC commands in FOpts / port 0.
4. Update ADR history (SNR and gateway margin per frame).
5. Decide whether a downlink is needed (confirmed uplink, pending MAC commands, queued app data).
6. Pass the decrypted payload to the application layer.

**Join server**

- Verify the join-request MIC with the device's root key.
- Reject any DevNonce not greater than the last stored one (1.0.4 rule).
- Assign a DevAddr from our own range, derive session keys, encrypt the join-accept with an EU868 CFList for channels 4–8.

**MAC commands for v1:** LinkCheck, LinkADR, DutyCycle, RXParamSetup, DevStatus, NewChannel, RXTimingSetup, DeviceTime.

**ADR:** ChirpStack-style algorithm — the margin between measured SNR and the required SNR for the current data rate steps the data rate up, then power down.

**Downlink scheduling**

- RX1 at uplink timestamp + 1 s (join-accept + 5 s), same channel, data rate offset per settings.
- RX2 on 869.525 MHz as the fallback.
- The gateway tracks its own transmit airtime per sub-band: 1% on most EU868 sub-bands, 10% on 869.4–869.65 MHz. A downlink that would exceed the budget moves to RX2 or is dropped and logged.

## Application layer and integrations

The gateway publishes standard formats so existing tools (Node-RED, Grafana, Home Assistant) connect without anything extra running on the P4.

**Payload handling:** decrypt FRMPayload with the device's AppSKey, then pass it to the codec (if one is set) and the integrations.

**Integrations**

| Integration | Direction | Format | Phase |
| --- | --- | --- | --- |
| MQTT | Out: uplinks, joins, status. In: downlink queue | ChirpStack-compatible JSON event topics | 3 |
| HTTP webhook | Out | Same JSON as MQTT, POSTed per event | 5 |
| Home Assistant | Out | MQTT discovery messages per device | 5 |

**Codecs:** decode and encode functions in JavaScript, compatible with ChirpStack and TTN codecs, run in an embedded QuickJS engine on PSRAM with a time and memory limit per call. Until then, raw payloads go out as base64 and are decoded downstream.

**Hybrid forwarding**

- A frame is local if its DevAddr is in our range (data uplinks) or its DevEUI is in our join table (join requests).
- Everything else is forwarded unchanged to TTN (Basics Station, ADR-0016) or a central ChirpStack (Semtech UDP).
- Downlinks from upstream share the same scheduler and duty-cycle budget as local downlinks, with local traffic taking priority.

## Management, OTA and observability

Everything you would do over SSH on a Pi has a replacement, and OTA comes early (phase 2) so every later phase ships remotely.

| On a Pi | On the P4 |
| --- | --- |
| `apt upgrade` | Signed A/B OTA from a URL or upload; new image must pass a health check within a set time or the bootloader rolls back |
| SSH | Web UI + REST API; USB serial console for recovery |
| Log files | Ring buffer in PSRAM, exported via syslog or MQTT; coredump partition fetched over the API |
| Database | LittleFS records, backup/restore as one file through the UI |
| systemd restarts | Task watchdog on every task; health task checks concentrator, network and MQTT and reboots cleanly if stuck |

**Web UI pages:** status (uplinks/min, last packet, backhaul, duty-cycle usage), devices (add, keys, codec, last seen), live frames, settings (channel plan, upstream server, MQTT), firmware, logs.

**REST API:** the same operations as the UI, so it can be scripted and the UI stays a thin client. Static assets served from LittleFS, gzip-compressed.

**Metrics** published on MQTT every minute: packets received/forwarded/dropped, CRC errors, downlinks sent/skipped for duty cycle, free heap/PSRAM, uptime.

## Security

The gateway holds every device's root keys, so a stolen unit or a flash dump must reveal nothing usable.

- **Secure boot v2:** only firmware signed with our key runs; OTA images are checked against the same key.
- **Flash encryption (release mode):** firmware, LittleFS and NVS are encrypted at rest; JTAG and UART download disabled on production units.
- **Key storage:** device root keys and session keys in encrypted NVS; never exposed in plain text through the UI or API after entry.
- **Gateway identity and mTLS:** a private CA (step-ca) issues each gateway a client certificate with its EUI as the CN. The private key is the ECC608 slot 0 key where the HAT has one, otherwise an ECDSA key held in P4 eFuse. Used for outbound MQTT to a central broker, with an ACL limiting each certificate to its own topics.
- **ECC608:** Nebra locked its configuration, so slot 0 is used as-is. Provisioning reads its public key and has the chip sign a CSR.
- **Web UI and API:** HTTPS with a device certificate, login required, separate admin and read-only roles.
- **Development builds** keep secure boot and flash encryption in development mode so boards can be reflashed.

## Connectivity

Ethernet is primary, Wi-Fi is automatic fallback, LTE is an optional add-on; the local network keeps running on any of them, or with none.

| Link | Hardware | Role | Phase |
| --- | --- | --- | --- |
| Ethernet 100 Mbps | Internal EMAC + IP101 PHY (RMII on GPIO28–35, 49–52) | Primary; PoE power | 1 |
| Wi-Fi 6 | ESP32-C6 over SDIO via ESP-Hosted | Fallback when Ethernet link drops; BLE setup through the C6 (ADR-0014) | 2 |
| LTE | USB modem on the USB host port, PPP via `esp-modem` | Optional for sites with no wired network | 6 |

**Failover:** the default route follows link state, Ethernet > Wi-Fi > LTE. MQTT and upstream UDP reconnect on route change; uplinks queue in PSRAM while all links are down.

**Time:** SNTP sets wall-clock time for logs and TLS certificate checks. Downlink timing uses the concentrator's own counter, so losing NTP does not affect LoRaWAN timing.

**C6 firmware:** the ESP-Hosted slave image on the C6 must match the host driver version; it is updated through the P4 as part of OTA.

## Build phases and milestones

Six phases, each closed by a gate; OTA and recovery (phase 2) come before any LoRaWAN logic so every later phase can be shipped and rolled back remotely.

&#91;embedded content: build roadmap · 6 phases, 5 gates\]

Phase 3 is the core of the project: once it passes, the board is a working stand-alone network and a credible Pi replacement. Phases 4 to 6 can be reordered to suit whichever site goes live first.

Planned main mode after phase 3: hybrid, with our own devices on the board and every other frame sent to TTN over Basics Station (ADR-0016), so the gateway also serves the community network. Basics Station is built straight after the phase 3 gate, before the phase 4 integrations.

CI — both chip-revision builds, static analysis, host tests and fuzzing — is set up in phase 1 and must be green at every gate.

## Risks and open questions

The biggest unknowns are in the hardware we're reusing, so phase 1 is built to surface them early.

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Recycled concentrators are SX1301, 915 MHz, or faulty | No usable radio | Inventory and test every card in phase 1; keep only 868 MHz SX1302/3 |
| HAL port timing or SPI issues on the P4 | Phase 1 slips | Bring up first against a Pi-tested HAT; compare against `sx1302_hal` test utilities on a Pi |
| PoE module can't supply concentrator transmit peaks | Brownouts during downlinks | Measure peak current; fall back to USB-C power or a higher-rated PoE splitter |
| P4 silicon revision vs ESP-IDF support | Build or runtime faults | Pin the ESP-IDF version; separate v1.3 and v3.x builds; check each board's revision before flashing |
| Flash wear from counters | Shortened device life | Batched writes and skip-ahead on boot; FRAM option |
| LoRaWAN edge cases in our own NS | Devices misbehave | ChirpStack as the reference; test against several device makes |
| Moving to Zephyr later costs a rewrite | Platform lock-in | OS-independent core library; only the platform layer is ESP-IDF specific |

**Open questions**

- [ ] Which Nebra HAT variant(s) do we have — original Hotspot HAT or Light Hotspot II?
- [ ] How many concentrator cards are SX1302/3 at 868 MHz?
- [ ] Peak current of the concentrator at full transmit power, and the PoE module's rating.
- [ ] Code reuse: our own C implementation of the NS, or ChirpStack's Rust `lrwn` crate on the P4?
- [ ] Does `esp-cryptoauthlib` support the P4 target as-is?
- [ ] Where does the central broker and step-ca run?
- [ ] Target number of devices per site (sizes ADR history and queue limits).
