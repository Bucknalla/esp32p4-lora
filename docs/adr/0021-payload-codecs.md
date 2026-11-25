# ADR-0021: Payload codecs in JavaScript (QuickJS-NG)

- Status: accepted (decoding and encoding built and tested on the board)
- Date: 2026-11-25

## Context

Uplinks go out as `payload_hex` (ADR-0018): every consumer has to know each
device's byte format. Device makers publish decoders in JavaScript for TTN
(`decodeUplink`, the TTN device repository) and ChirpStack. Running those
same files on the gateway lets an operator add a device type without a
firmware release. The design plan names an embedded QuickJS engine with
per-call limits. The owner chose this over built-in C decoders.

## Decision

- **Engine:** `espressif/quickjs-ng` 0.14.0, Espressif's port of QuickJS-NG
  (MIT; the port layer Apache-2.0), pinned exactly. Host tests build
  upstream QuickJS-NG v0.14.0 (fetched by CMake, SHA-256 pinned); the two
  differ in formatting and spelling only (checked: astyle and codespell
  changes, no code changes found when whitespace, braces and comments are
  ignored). It adds about 515 KB to the app (2.19 MB of 4 MB).
- **Only the core language:** no `std`/`os` modules, so a codec has no file,
  network, timer or process access.
- **Compiled once, run fresh:** a codec is compiled to bytecode when it is
  stored (and at boot). Every decode runs in a new runtime and context built
  from that bytecode, so nothing carries over between calls or devices, and
  memory is all returned. Measured on the board (rev3): 15-29 ms per decode
  for codecs of 1.6 to 51 KB (a 51 KB source compiles to 75 KB of bytecode;
  compiling it on every call would take 90 ms).
- **Limits per call:** 1 MB of PSRAM, 200 ms (setup included), 32 KB of C
  stack; compiling: 2 s. On the board: an endless loop stops at about
  204 ms, a deep recursion with a RangeError, a memory bomb with "out of
  memory", a catastrophic regex by the time or stack limit. (The figures
  first given, 50 ms and 64 KB, were too low: a 51 KB codec peaks at
  167 KB and runs about 24 ms.)
- **One `codec` task** runs all JavaScript, on a 48 KB stack in PSRAM (it
  never touches flash, so it only pauses while another task writes flash);
  the MQTT task (decodes) and the web server (uploads, tests) post requests
  and wait. It is the only writer of the table of compiled codecs. The
  supervisor restarts the gateway if it stops answering for 30 s.
- **Formats:** `decodeUplink(input)` (TTN v3, ChirpStack v4; `input` has
  `bytes`, `fPort` and, with the clock set, `recvTime`; its `errors` fail the
  decode, its `warnings` are passed on), else `Decoder(bytes, port)` (TTN v2),
  else `Decode(fPort, bytes, variables)` (ChirpStack v3). The data must be a
  plain object, not a Promise; it is serialised by `JSON.stringify`, at most
  1 KB, and checked again as one JSON object before it goes into a message.
- **Encoders** (downlinks): `encodeDownlink(input)` (TTN v3, ChirpStack v4;
  `input` has `data`, `fPort` when the request gave one, and `variables`;
  its `errors` refuse the downlink), else `Encoder(object, port)` (TTN v2),
  else `Encode(fPort, object, variables)` (ChirpStack v3). The bytes must be
  an array (or a Uint8Array) of integers 0-255, at most 242; the FPort is
  the codec's (1-223), else the request's, else the downlink is refused.
- **A codec may hold a decoder, an encoder or both.** Makers that publish
  them as two files (Milesight) are stored joined, as one codec per device.
- **Downlink JSON** (ADR-0018): `"object": {...}` instead of `payload_hex`
  (exactly one of the two; at most 1 KB; `port` optional). The esp-mqtt
  task has the device's codec encode it (through the codec task, so it may
  wait up to the 200 ms limit), then queues the bytes as usual. The results
  carry the encoded port and length; a refusal carries the codec's own
  message ("encoder errors: ...", "the device has no codec ...").
- **Uplink JSON:** with a codec, `"codec"`, then `"object"` (the data),
  `"codec_warnings"` or `"codec_error"`. `payload_hex` is always there, so a
  broken codec never hides the bytes. Port 0 (MAC commands) is not decoded.
- **Storage:** the `storage` partition (5.75 MB) as littlefs
  (`joltwallet/littlefs` 1.22.3, pinned), `/storage/codecs/<name>.js`. Up to
  16 codecs of at most 128 KB (TTN's Milesight decoders reach 51 KB; their
  encoders 120 KB). A script that does not compile is never stored. A
  partition that does not mount is formatted only if it is blank (all 0xFF);
  otherwise codecs stay off and the gateway runs on. A stored codec that no
  longer compiles (after an engine update) is listed with its error.
- **Which codec a device uses** is kept per device slot in NVS (key
  `x<slot>` in namespace `lns`), erased when the device is removed. Deleting
  a codec in use leaves the devices with `codec_error` ("no codec named ...")
  until another is chosen.
- **API:** `GET /api/codecs` (viewer), `GET /api/codecs/source?name=`,
  `POST /api/codecs?name=` (the script as the raw body), `POST
  /api/codecs/delete`, `POST /api/codecs/test` (decode a payload once) and
  `codec=` on `POST /api/devices/settings` (admin). The Devices tab has a
  codec list, upload, test pane and a codec per device.
- **Logging:** codec failures are logged at most once a second.

## Consequences

- An operator adds a device type by uploading the maker's decoder; consumers
  get named values without their own decoders.
- Decoding happens in the MQTT task's path: a slow codec delays that
  device's uplink message by up to 200 ms; the radio path is not affected.
- Anyone who can publish on the broker can make the gateway run a device's
  encoder (up to 200 ms each, on the codec task at low priority, never on
  the radio core): limit publishing with the broker's ACL (ADR-0018).

## Verification (2026-11-25, rev3 bench board, desk build)

| Test | Result |
| --- | --- |
| Host tests (`test_codec`, ASan/UBSan, every allocation counted: the three formats, warnings, errors, recvTime, isolation between calls; syntax error, no decoder, a throwing top level, an endless top level; non-object data, arrays, async, thrown non-Errors, a throwing toString, BigInt, output too large, an endless loop, a looping toJSON, a catastrophic regex, deep recursion, a memory bomb; `test_mqtt`: codec fields, output that is not one object never embedded) | pass, nothing left allocated |
| First boot with the new storage | partition blank, formatted, mounted (16 of 5888 KB) |
| Uploads: em300-th (1.6 KB), lsn50-v2 (7.7 KB), wt303 (51 KB) from the TTN device repository, a FiPy codec; a syntax error; a bad name | stored; refused with the syntax error; refused |
| Test decodes | 16, 21, 29, 16 ms; warnings and decoder errors reported; an endless loop stopped at 204 ms; a deep recursion: RangeError (codec stack 12 KB left of 48); a memory bomb: out of memory; an unknown codec |
| 40 decodes (20 hitting the time limit) | internal and PSRAM heap unchanged |
| FiPy with its codec, 3 uplinks | each `up` message has `"codec":"fipy","object":{"counter":n,"text":"mqtt"}`, nothing dropped; codecs reloaded after the restart |
| FiPy with an endless-loop codec, 3 uplinks | each published with `codec_error` "decodeUplink: time limit reached", nothing dropped, one log line each |
| Assigning a codec that does not exist | 400 |
| Host tests (`test_codec` encode: the three formats, fPort from the codec or the request, a Uint8Array, an empty payload, 242 bytes; refused: decoder-only codec, encoder errors, 11 kinds of bad bytes (256, -1, 1.5, "1", null, a string, NaN, Infinity, 243 bytes, ...), 5 bad ports, async, an endless loop, a looping getter, a non-object; `test_mqtt`: `object` kept exactly, port optional, not both, 1 KB limit) | pass, nothing left allocated; UBSan found a NaN-to-int cast, fixed |
| Test encodes (FiPy codec with an encoder) | `{"counter":7,"text":"hi"}` -> port 30, `076869` in 15 ms; encoder errors, a codec without an encoder and a non-object refused with clear reasons |
| FiPy: object downlink (confirmed), one the encoder refuses, and a payload_hex one | FiPy received `076869` on port 30 and then `0102`; results o1 queued (port 30, 3 B), sent, acked; o2 refused "encoder errors: counter: 0 to 255"; u1 queued, sent |
