# ADR-0018: MQTT client

- Status: accepted (events out and downlinks built and tested on the board)
- Date: 2026-11-24

## Context

The design plan (`docs/design-plan.md`, integrations table) lists MQTT for
uplinks, joins and status out, and a downlink queue in. The gateway is
infrastructure: the broker and the applications behind it belong to the
operator, and the gateway must not assume which application consumes the
data. The first bench broker is a Mosquitto instance on the local network
(plain TCP 1883, user and password, no TLS).

A ChirpStack layout (`application/<id>/device/<eui>/event/up`, base64 data)
needs an application id the gateway does not have, and has no consumer
today.

## Decision

- **A generic MQTT client.** Events out and downlinks in, as plain JSON on
  `lora/...` topics. No application-specific discovery or configuration
  topics: an application subscribes to the topics it needs. The Setup step
  is "Connect to an MQTT broker".
- **Topics**
  - `lora/<gateway_id>/status`: `online` / `offline`, retained, QoS 1. Also
    the last will, so the broker marks the gateway offline when it vanishes.
  - `lora/<gateway_id>/<node_id>/up` and `.../join`: JSON, QoS 0, not
    retained.
  - `lora/<gateway_id>/<node_id>/down`: downlinks in (below).
  - Ids are `[a-z0-9_]`, 1-31 characters; the gateway rejects anything else
    (no `/ + #` can reach a topic).
- **`node_id` is the DevEUI in lower-case hex**, not the device name, so a
  rename never changes a device's topics. The name is in the JSON.
- **`gateway_id`** defaults to the gateway EUI in lower case; it can be set
  on the Setup tab. Changing it changes every topic. Client id
  `lora-gw-<last 6 hex of the EUI>`, clean session, keepalive 60 s.
- **Uplink JSON** (core, `lwc/mqtt.h`, host-tested): `node_id`, `name`,
  `dev_eui`, `dev_addr`, `fcnt`, `port`, `confirmed`, `payload_hex`, `rssi`,
  `snr`, `sf`, `bw_khz`, `freq_hz`, `ts` (UTC, only when the clock is set).
  There are no decoded fields: the gateway has no payload codecs yet.
- **Only the local network server's own devices.** Events: accepted uplinks
  (`LWC_NS_UP_OK`, not retransmissions) and answered joins. Traffic forwarded
  to TTN in hybrid mode is not published.
- **Never block the radio path.** The `lns` task copies the event into a
  16-entry PSRAM queue with a zero timeout, after the downlink is sent. A
  separate `mqttc` task publishes. When MQTT is off, not connected or the
  queue is full, the event is dropped and counted. No backlog is kept while
  the broker is away (QoS 0).
- **Client:** `espressif/mqtt` 1.1.0 from the component registry (esp-mqtt
  left ESP-IDF in 6.0). Buffers 1536 B, outbox limit 32 KB in PSRAM,
  reconnect every 10 s. Library logging is off; the gateway logs each new
  failure once, so retries do not fill the log ring.
- **Settings** go through `/api/settings` (no new handlers) and are stored in
  NVS namespace `mqtt`. The password is write-only: never in the status, the
  logs or a URI.
- **Downlinks in:** the gateway subscribes to `lora/<gateway_id>/+/down`
  (QoS 1) after every connect. A message is
  `{"id":"..","port":1..223,"payload_hex":"..","confirmed":false}` (parsed
  in core, host-tested and fuzzed; `confirmed` and `id` optional; a wrong
  field does not stop the reading, so the `id` is in the `refused` result
  wherever it is). It is
  queued for that local device and sent with its next class A answer. The
  queue rules are in core (`lwc/dlq.h`, host-tested).
  - Retained and empty messages are ignored: a retained downlink would be
    sent again after every reconnect. (A broker delivers a new retained
    message once, live, to current subscribers; that copy is used.)
  - Up to 4 per device, in PSRAM, lost on restart. Keyed by DevEUI too, so a
    reused device slot never sends an old payload; removing a device empties
    its queue. FPending is set while more are waiting.
  - The head is settled by the device's next new uplink. A repeated uplink
    (the device missed the answer) gets it again with a new FCntDown. If the
    radio refused the frame or could not hand it over (duty-cycle budget,
    too late, a clash, a missed start), the TX task reports it without
    waiting and the payload is offered again.
  - **Confirmed downlinks** (`"confirmed":true`) go out as Confirmed Data
    Down. The device's next new uplink settles it: FCtrl.ACK set is `acked`,
    not set is `nacked`. A nacked downlink is never sent again on its own:
    if only the device's ACK uplink was lost, a resend would run the command
    twice (a valve, a relay). The application decides about a retry and can
    match it by `id`. A downlink is never repeated with the same counter. The ACK bit of a repeated
    uplink belongs to the earlier frame and is ignored.
  - Switching local transmit off drops what is waiting (counted), so nothing
    goes out late when it is switched on again.
  - Clean session: a downlink published while the gateway is not connected
    is lost.
  - Size: the same frame goes to RX1 and the RX2 fallback, so it must fit
    the smaller of the two (EU868 RP002 N: 51 B at DR0-2, 115 B at DR3, 222
    B at DR4-7), less the MAC answers in FOpts. Larger than the RX2 window:
    refused at once. Too large at this uplink's data rate: dropped and
    counted, or it would block the queue. The daily DevStatusReq is left out
    when it would crowd out the payload.
  - Refused at once (counted in the status; logged at most once a second,
    so a flood cannot empty the log ring): bad JSON, port 0 or above 223,
    odd or non-hex payload, a bad `id`, unknown device, queue full, local
    transmit off (the release default).
- **Downlink results** on `lora/<gateway_id>/<node_id>/down/result`, QoS 1,
  not retained, with the caller's `id`, port, length, `confirmed`,
  `fcnt_down` and `tx` (transmissions) where known, and `ts`:
  - `queued`, or `refused` with a `reason`;
  - `sent` each time the frame is handed to the concentrator for its receive
    window (the last word for an unconfirmed downlink);
  - `acked` or `nacked` for a confirmed downlink;
  - `dropped` with a `reason`: too big for the uplink's data rate, local
    transmit switched off, device removed.
  - They have their own 8-entry queue, so a flood of bad downlinks never
    takes room from uplink events; `refused` results may fill only half of
    it, so results of queued downlinks keep room. Like events, results are
    best-effort:
    lost while the broker is away, and dropped (counted:
    `results_dropped`) when the queue is full.
  - The topic does not match the `+/down` subscription, so the gateway
    never reads its own results.
  - Counters: `down_queued`, `down_refused`, `results`, `results_dropped`
    (mqtt) and `app_waiting`, `app_queued`, `app_sent`, `app_acked`,
    `app_nacked`, `app_dropped` (lns).
- **Plain MQTT** here; TLS with server authentication is ADR-0019. The
  design plan's central-broker mTLS stays open.

## Consequences

- Any MQTT application (a flow engine, a time-series pipeline, a building
  management system, own code) consumes the same topics; nothing on the
  gateway is specific to one of them.
- Anyone who can publish on the broker can send downlinks to local devices.
  Limit that with the broker's ACL (the Setup tab says so).
- Decoded values (temperature and so on) need payload codecs; until then
  consumers decode `payload_hex` themselves.
- A second consumer that wants ChirpStack JSON needs another topic layout;
  `lwc/mqtt.h` keeps the JSON in core, so a second formatter is small.
- Changing the gateway id leaves the old id's retained status topic on the
  broker; clear it there.

## Verification: events out (2026-11-24, rev3 bench board, desk build, Mosquitto broker)

Run on the first build, which also published application-specific discovery
topics (since removed); the results below are those that do not depend on
them. The stack figure is from that build.

| Test | Result |
| --- | --- |
| Host tests (`test_mqtt`: ids, ISO 8601, topics, exact JSON, escaping, overflow, 242-byte payload) | pass |
| 7 bad settings (port 0, `a/b`, capitals, spaces, 70-character password, `mqtt=maybe`, user with a space) | 400 each |
| Login refused by the broker (CONNACK 5) | state `refused`, one log line, password not in logs or status |
| User `a&b+c%d=e` through the form | read back unchanged |
| Connect | `online` retained |
| FiPy join + 3 uplinks (one confirmed) | `join` and `up` messages, fcnt 0-2, `confirmed` true on the third |
| Gateway held in the ROM bootloader | broker sent `offline` after ~88 s (last will) |
| Reboot | `online` again 19 s after reset |
| Session taken over (same client id) | `offline`, then `online` again 16 s later; `connects=2`; retained status `online` |
| Broker restarted | reconnected 15 s after the drop, retained status `online`; no restart of the gateway (uptime 72 min) |
| Broker unreachable (wrong port), FiPy join + 3 uplinks | `dropped=4`; join answered and confirmed uplink acked |
| Stacks after the tests | `mqttc` 3012 of 4096 B free, `mqtt_task` 5076 of 6144 B free |

## Verification: downlinks (2026-11-24, same bench)

| Test | Result |
| --- | --- |
| Host tests (`test_ns`: frame decoded as the device reads it, MIC, decrypt, FPort, FPending, 115/116 B at DR3, 51/52 B with RX2 DR0, MAC answers sharing the room, RETRANS, DevStatusReq crowding, empty payload, ports 0 and 224; `test_mqtt`: topics and JSON) | pass, with ASan/UBSan |
| Fuzz smoke of the downlink parser (2 M mutated inputs) | no fault |
| 8 bad messages (confirmed, port 0, odd hex, not JSON, no payload, unknown device, 52 B for RX2 DR0, queue full) | refused with a reason, one log line each |
| RX2 set to DR3, FiPy joined again, 116 B then 115 B published | 116 B refused (RX2 window); 115 B received by the FiPy on port 21, bytes exact |
| Status after the tests | `app_queued`, `app_sent`, `app_dropped` count as expected; stacks `lns` 4392, `mqtt_task` 4724, `httpd` 7192 B free |
| 3 downlinks queued (1 B port 9, 5 B port 7, 51 B port 8), FiPy join + 4 uplinks | FiPy received each, in order, with the right port and bytes; FPending on the first two; nothing on the fourth |
| Local transmit off | refused: "local transmit is off" |
| Retained downlink, then reconnect | used once live; ignored on reconnect |
| Empty message (clearing a retained one) | ignored, no log line |

## Verification: application-neutral build (2026-11-25, same bench)

| Test | Result |
| --- | --- |
| Host tests, cppcheck, fuzz smoke of the downlink parser (2 M inputs) | pass, no fault |
| Connect | `published=1`: only `lora/<gateway_id>/status` `online` |
| Subscribed to `lora/#` and the common discovery prefixes across two restarts | `status` and the downlinks only; nothing else published |
| Reboot | `offline` (last will), then `online` |
| Downlink for a local device (2 B, port 5), and one with port 0 | first queued (`down_queued=1`, `app_waiting=1`); second refused, "port: 1 to 223" |
| Settings and status | no discovery or expiry fields left |
| FiPy join + 3 uplinks (third confirmed) | `join` and 3 `up` messages, fcnt 0-2, `confirmed` true on the third; `published=5`, `dropped=0` |

## Verification: confirmed downlinks and results (2026-11-25, rev3 bench board, desk build)

| Test | Result |
| --- | --- |
| Host tests (`test_dlq`: release, repeat, radio refusal, ACK and NACK, stray ACK, a burst of 7 repeats, slot reuse, full queue, ids; `test_ns`: MType 5 decoded as the device reads it, the uplink's ACK bit; `test_mqtt`: `id`, result JSON, the `down/result` topic) | pass, with ASan/UBSan |
| Confirmed `c1` (3 B, port 21) and unconfirmed `u1` (2 B, port 22) published after the FiPy joined; FiPy sends 4 uplinks | FiPy received both in order; first with FPending. Results: `c1` queued, sent (fcnt_down 0, tx 1), acked; `u1` queued, sent; nothing on the last two uplinks |
| 350 bad downlinks published during the run | all 4 uplinks published, `dropped=0`; `results_dropped=285` (the result queue's cap); 2 log lines |
| Stacks after the tests | `radio` 7060, `lns` 4312, `mqttc` 3048, `mqtt_task` 4888 B free |

`nacked` is host-tested only: the FiPy always acknowledges.

## Plan

1. Events out (done).
2. Downlinks: `lora/<gateway_id>/<node_id>/down` into the local downlink
   queue (done), confirmed downlinks and results (done).
3. Payload codecs, then decoded fields in the JSON.
