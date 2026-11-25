# ADR-0019: MQTT over TLS (server authentication)

- Status: accepted (built and tested on the board)
- Date: 2026-11-25

## Context

ADR-0018 ships plain MQTT: the broker password and every payload cross the
network readable, so it is fit only for a closed local network. An
infrastructure deployment needs the link encrypted and the broker
authenticated. The owner chose: brokers with certificates from a public CA
(Let's Encrypt), user and password first, client certificates (mTLS) later,
and plain MQTT kept as an option.

## Decision

- **A switch, `mqtt_tls=on|off`** (Setup tab "Use TLS", stored in NVS
  namespace `mqtt`, key `tls`; default off, so existing settings keep
  working). Plain MQTT on 1883 stays available.
- **Server authentication against the ESP-IDF CA bundle** (the same bundle
  OTA and the Basics Station link use): any broker certificate from a public
  CA, Let's Encrypt included, checked for its validity and for the host name
  set on the Setup tab. There is no "skip verification" option and no custom
  CA upload yet: a private CA comes with mTLS.
- **User and password stay** as the client's credentials; with TLS they are
  no longer readable on the network.
- **Port**: the UI moves 1883 to 8883 when TLS is switched on (and back),
  unless another port was typed. The gateway does not guess ports.
- **Host name, not an IP address**, for TLS: a public CA certificate names
  a host. An IP address is accepted (some CAs issue IP certificates), but
  the Setup tab says to use the name.
- **Failures are told apart** in the status (`state=tls_error`) and logged
  once per kind:
  - the certificate was not accepted (`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`:
    not from a public CA, expired, or another name);
  - another handshake failure, with the mbedTLS code ("is TLS on at this
    port?").
  The Overview and the Setup step show it.
- **Quiet transport logs**: esp-tls and the transport base log three error
  lines on every failed retry (every 10 s). Their tags are set to none, in
  both the MQTT client and the Basics Station link, which each log their own
  failure once with the esp-tls and mbedTLS codes (the Basics Station line
  now carries the mbedTLS code and certificate flags as well).
- **esp-mqtt task stack 8 KB with TLS** (6 KB without).

## Consequences

- A broker on the local network needs a public host name with a Let's
  Encrypt certificate (for example via DNS validation), and the gateway must
  resolve that name to the broker.
- Two TLS sessions can be open at once (Basics Station and MQTT), each with
  mbedTLS buffers in internal RAM (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`, 16 KB
  input records).
- Certificate validity needs the wall clock: until SNTP has set it, a
  handshake may fail and is retried every 10 s.
- Client certificates (mTLS), a private CA and where the key lives (ECC608,
  P4 eFuse ECDSA, or encrypted NVS) are the next step.

## Verification (2026-11-25, rev3 bench board, desk build)

| Test | Result |
| --- | --- |
| TLS on, against the broker's plain port 1883 | `state=tls_error`; one line: "TLS handshake with the broker failed (mbedTLS -0x7280): is TLS on at this port?" |
| TLS on, against a self-signed `openssl s_server` | refused (the server saw "bad certificate"); `state=tls_error`; one line: "broker certificate not accepted ..." |
| Transport log lines during 5 minutes of failed TLS retries | 0 (was 3 every 15 s); the Basics Station link still connected |
| Stacks and heap during the failed handshakes | `mqtt_task` 6692 of 8192 B free; internal heap 205 KB free, lowest 196 KB, with the Basics Station session up |
| Public test broker `test.mosquitto.org:8886` (Let's Encrypt YR2 → Root YR, cross-signed by ISRG Root X1 in the bundle), no login, a random gateway id, local transmit off | connected over TLS; retained `online` seen by a subscriber; a downlink sent through the broker came back as `refused` ("local transmit is off") with its `id`; retained topic cleared afterwards |
| Stacks and heap with both TLS sessions up (Basics Station and MQTT) | `mqtt_task` 5928 of 8192 B free, `mqttc` 2536 of 4096 B free; internal heap 180 KB free, lowest 153 KB |
