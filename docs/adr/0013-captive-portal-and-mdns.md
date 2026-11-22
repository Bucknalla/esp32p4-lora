# ADR-0013: Captive portal for setup, and an mDNS name

- Status: superseded by ADR-0014 (the captive portal (DNS and port 80); the mDNS name stays)
- Date: 2026-11-22

## Context

With the setup access point (ADR-0011) the user had to know to type
`https://192.168.4.1/`, and after setup to find the gateway's new address in
the router. Phones open a sign-in sheet when a connectivity probe on a new
network does not get the expected answer.

## Decision

- **Captive DNS** (setup mode only): a UDP server on port 53 answers every A
  query with 192.168.4.1 and other types with an empty answer
  (`core/src/dns.c`, host-tested and fuzzed; the query is untrusted).
- **Port 80** (setup mode only): every request, including the phones' probes
  (`captive.apple.com/hotspot-detect.html`, Android `generate_204`, ...),
  is redirected to `https://192.168.4.1/`, so the sign-in sheet opens the
  secure setup page directly.
- **Plain fallback** at `http://192.168.4.1/plain`, for a sheet that cannot
  accept the self-signed certificate: a script-free form for the first-time
  claim only, with a warning that the passwords cross the open network
  unencrypted, the admin password asked twice, network names HTML-escaped,
  Origin checked.
- **Network choice:** both forms list the scanned networks (strongest first,
  duplicates removed) plus "Other network" for a hidden one.
- **No DHCP option 114**: RFC 8910 wants the URI of a captive-portal API
  (JSON over trusted HTTPS, RFC 8908), which this gateway cannot offer; the
  probe redirect is enough.
- **mDNS** (`espressif/mdns`): hostname `lorawan-gateway-xxxxxx` (last 3 MAC
  bytes) with an `_https._tcp` service, on Wi-Fi, Ethernet and the setup AP.
  The setup success messages point to `https://lorawan-gateway-xxxxxx.local/`.
  New certificates use that name as CN and SAN (existing ones keep theirs
  until the next reset).

## Consequences

- During setup, phone apps whose lookups now resolve to the gateway fail TLS
  against it; those log tags are silenced in setup mode.
- `.local` names need an mDNS resolver: iOS, macOS, Android 12+ and current
  Windows have one.

## Verification (2026-11-22, bench board, user's iPhone)

| Test | Result |
| --- | --- |
| Join the setup AP | address 192.168.4.2 from the gateway |
| iPhone probe | `GET /hotspot-detect.html (host captive.apple.com)` → redirect; the sign-in sheet opened the secure page |
| Setup on the secure page | "Wi-Fi stored and admin password set"; restart; joined the network; TTN link up |
| Earlier build (portal page before the redirect) | the user completed setup through it too |
| mDNS from the Mac | `lorawan-gateway-xxxxxx.local` resolves; `https://…local/` 200; `_https._tcp` browse lists "LoRaWAN Gateway" |
| Plain fallback `/plain` | built and served; not used on a phone |
