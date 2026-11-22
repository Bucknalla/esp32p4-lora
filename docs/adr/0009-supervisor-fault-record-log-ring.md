# ADR-0009: Software watchdog, fault record and retained log ring

- Status: accepted
- Date: 2026-11-22

## Context

Phase 2 asks for a watchdog on every long-lived task, reset reasons and the
last fault kept and reported on the next boot, and logs that can be read
after the fact. The hardware task watchdog (TWDT) has one global timeout
(5 s) and panics; several tasks legitimately block much longer: a DNS lookup
that times out, up to three TLS connects with a 15 s timeout during an OTA
check, `lgw_start` (~3.2 s) and the concentrator recovery backoff (up to
60 s). Rollback (ADR-0008) only protects the first minutes of a new image.

## Decision

- **Supervisor (`sv.c`, core `lwc_sv_*`).** Each long-lived task registers
  when it enters its loop, with its own budget, and beats while it makes
  progress; waits are cut into slices of at most 1 s (10 s for the OTA poll
  wait) that beat. Budgets: radio 30 s, console 30 s, main 30 s, pktfwd 90 s,
  ota 180 s. The supervisor task (core 0, priority 6) feeds the TWDT itself,
  so it is covered too. A missed budget writes a fault note and calls
  `esp_system_abort()`: a panic, so the coredump holds every task's stack.
- **Reboot-loop guard.** After 5 abnormal resets in a row (panic, watchdog,
  brownout) the supervisor only logs and stops watching that task, so the
  OTA task can still bring a fix. The run is cleared after 10 minutes of
  uptime and by a power cycle.
- **Fault record (`fault.c`).** A note ("watchdog: pktfwd stalled 91 s",
  "OTA installed v0.2.1", "console Wi-Fi change", ...) is written to RTC
  no-init memory before every deliberate restart or abort: no flash write on
  the fault path. Counters per reset category (boots, panic, watchdog,
  brownout, software, other) live in NVS, one write per boot. The console
  `STATUS` command prints them.
- **Log ring (`logbuf.c`, core `lwc_logring_*`).** An `esp_log` hook copies
  every log line into a 64 KB ring in PSRAM `.noinit` memory and pushes the
  written bytes from cache to PSRAM (`esp_cache_msync`). At boot the
  previous boot's last 8 KB is kept; after an abnormal reset its last ~3 KB
  is printed. The retained header is untrusted input: magic, size, indices
  and CRC are checked, and non-printable bytes are masked (fuzzed in CI).
  The hook never blocks: a line that meets a held lock is skipped in the ring
  (it still goes to the UART).
- **Dev and release builds.** `tools/build.sh` builds dev images by default:
  automatic install of new releases off (`OTA_CHECK` still installs) and the
  console test commands `TEST_ABORT` and `TEST_STALL` on. `tools/ota_release.sh`
  builds with `GW_RELEASE=1` (auto-install on, test commands off) and refuses
  to publish anything else.

## Consequences

- A wrong budget that fires on a valid image would loop until the guard
  stops it after 5 resets; budgets were set from the worst legitimate block
  and the outage tests below.
- The first bytes of PSRAM are overwritten at boot on this board (measured:
  the first 8 bytes at `0x48000000`, probably MSPI timing tuning). The ring
  therefore starts 4 KB into its section.
- Log lines from before `app_main` (bootloader, early start-up) are not in
  the ring. A power cycle clears the ring and the note; NVS counters remain.
- Tasks owned by ESP-IDF and ESP-Hosted are not registered; the TWDT idle
  checks still catch a starved CPU.
- Coredumps are kept in flash and summarised at boot; fetching them over the
  network comes with the web/API work.

## Verification (2026-11-22, bench board)

| Test | Result |
| --- | --- |
| `TEST_ABORT` | panic, coredump (task `console`), next boot: note "console TEST_ABORT", 4.9 KB of previous log kept and printed, panic counter +1 |
| First try of the ring | 0 bytes kept: the header's first 8 bytes were overwritten during boot; fixed with the 4 KB guard |
| `TEST_STALL` (console stops beating) | supervisor abort after 30 s naming `console`; coredump decodes (`idf.py coredump-info`) with the console task stopped in `handle_line` |
| Outage build: forwarder server `gw.invalid`, OTA URL `https://192.0.2.1/...`, poll every minute, 12 min | no abort, no reset; 15 DNS failures (fast), 10 TLS connect timeouts (15 s each, inside the 180 s budget); internal heap min 337 KB |
| Soak, normal dev build, 31 min (two OTA poll cycles) | no abort, no reset; 60/60 TTN stats acknowledged; internal heap 348 KB free (min 298 KB); abnormal-reset run cleared after 10 min |
| Least free stack with the log hook | console 2884 B (of 4096), main 2708 B, pktfwd 5132 B, ota 7108 B, radio 7288 B |
| Reboot-loop guard: `TEST_STALL` six times in a row | stalls 1–5 aborted (coredump, reboot); the 6th logged "abnormal run 5: not aborting" and the gateway kept running; a web UI reboot cleared it. Coredump downloaded over HTTPS decodes (`esp-coredump info_corefile -t raw`) |
