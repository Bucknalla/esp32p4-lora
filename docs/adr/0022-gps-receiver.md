# ADR-0022: GPS receiver (NMEA)

- Status: accepted (parser and clock rule host-tested, also on the owner's NMEA simulator's output; no receiver wired yet)
- Date: 2026-11-25

## Context

A GPS receiver gives a gateway its position and, with a PPS line into the
concentrator, a time reference for class B beacons, `gpstime` in Basics
Station and fine timestamps. Without a network it is also the only source of
wall-clock time, which DeviceTimeAns needs in standalone mode. The bench has
no receiver yet, and which module, which header pins and whether its PPS
reaches the concentrator are not confirmed; the HAL's `loragw_gps.c` is
Linux-only (termios) and is not built.

## Decision

- **NMEA in core** (`lwc/gps`, host-tested and fuzzed, `fuzz_gps` in CI): RMC
  and GGA from any talker (GP, GN, GL, GA, BD, ...), the checksum required,
  at most 82 characters, every field checked (time, date, hemisphere,
  minutes below 60, degrees in range); a bad sentence changes nothing. Empty
  fields (no fix) are read as no fix. Two-digit years 80-99 are 1980-1999.
- **Setting `gps=off|uart`** (`/api/settings`, NVS namespace `gps`, the
  Radio tab), applied at the next start; default off. `uart`: UART2 at
  `CONFIG_GW_GPS_BAUD` on `CONFIG_GW_GPS_UART_RX/TX_GPIO`. Both default to
  -1 (not wired), and then `uart` is refused: there are no guessed pins.
- **No receiver mock in the firmware.** For tests without a receiver, the
  owner's NMEA simulator (`go-gps-simulator`, GGA, RMC, GLL, VTG, GSA, GSV,
  ZDA, lock time, jitter) runs on a computer and writes to a 3.3 V
  USB-to-UART adapter wired to the RX pin (`-serial`, `-baud`). That tests
  the pins, the baud rate and the clock setting too; a mock inside the
  firmware could not, and was removed.
- **Wall clock from a fix** (`lwc_gps_clock_set`, host-tested): only with
  RMC status A and a date from 2025 to 2099
  (an old receiver's week rollover gives dates about 19.6 years back); SNTP
  synced in the last hour wins; a clock that is not set is set; one kept by
  GPS, or by an SNTP that went quiet, is corrected when 2 s or more away (so
  it does not drift for days). NMEA gives UTC; the wall clock is UTC
  (`lwc_gps_from_unix_us` adds the 18 leap seconds). The status says which
  source set the clock (`clock=none|sntp|gps`).
- **No PPS and no counter time reference yet.** An NMEA sentence arrives an
  unknown time after its second, so it cannot anchor the concentrator
  counter; that needs the PPS edge latched by the concentrator
  (`lgw_get_trigcnt`, read in the radio task). Class B, `gpstime` and fine
  timestamps wait for a receiver with PPS.
- **The position stays out of the log** and out of the diagnostics report
  (removed from it, not only redacted); it is in the status for logged-in
  users.

## Consequences

- With a receiver (or the simulator) wired, setting the Kconfig pins and
  `gps=uart` gives a position and an offline clock.
- TTN class C downlinks do not need GPS (see the correction in ADR-0020).
- Until a receiver or the simulator is wired, the firmware path (UART,
  pins, clock setting) is untested on the board; the parser and the clock
  rule are host-tested.

## Open questions (for the owner)

- Which GPS module (or the HAT's), on which header pins, at what baud rate?
  (For the simulator: which free header GPIO for its adapter.)
- Does its PPS reach the concentrator's GPS/PPS input?

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| Host tests (`test_gps`: RMC and GGA from GP and GN talkers, south and west, fractions of a second, negative altitude, no-fix sentences, ignored sentences; 17 bad ones incl. checksum, missing `$`, status A without a date, hemisphere, 60 minutes, 24 h, day 32, 91 degrees, a control character, too long; a refused sentence changes nothing; build and parse round trip; dates incl. a leap day; every clock rule) | pass, with ASan/UBSan |
| The simulator's output (6 s at a public test position: 44 sentences of 7 types, no fix then a lock), through the parser; 24 of its lines kept as a host test | 0 bad; the V-then-A lock read; position, altitude, 8 satellites and UTC as sent |
| Fuzz smoke (2 M mutated sentences, checksums fixed half the time) | no fault |
| Board (rev3, desk build): `gps=uart` with no pins; `gps=bogus` | 400 with the reason; 400 |
| Board, with a firmware mock since removed: UART path, parser, status, UI | fix read, 0 bad, the clock not set, no position in the log; `gps` task 3060 of 3968 B stack free |
