# ADR-0018: MQTT events to Home Assistant

- Status: accepted (events out built and tested on the board; downlinks later)
- Date: 2026-11-24

## Context

The design plan (`docs/design-plan.md`, integrations table) lists MQTT out
with "ChirpStack-compatible JSON event topics", and Home Assistant discovery
as a later step. The first real consumer is a Home Assistant instance with
the Mosquitto app (plain TCP 1883, user and password, no TLS). The owner's
brief for that integration sets the topics, the discovery layout and the ids.
A ChirpStack layout (`application/<id>/device/<eui>/event/up`, base64 data)
adds nothing for Home Assistant and has no consumer today.

## Decision

- **Topics follow the Home Assistant brief, not ChirpStack.**
  - `lora/<gateway_id>/status`: `online` / `offline`, retained, QoS 1. Also
    the last will, so the broker marks the gateway offline when it vanishes.
  - `lora/<gateway_id>/<node_id>/up` and `.../join`: JSON, QoS 0, not
    retained.
  - Ids are `[a-z0-9_]`, 1-31 characters; the gateway rejects anything else
    (no `/ + #` can reach a topic).
- **`node_id` is the DevEUI in lower-case hex**, not the device name. A
  rename must never change a Home Assistant `unique_id`. The name is in the
  JSON and in the Home Assistant device name ("LoRa <name>").
- **`gateway_id`** defaults to the gateway EUI in lower case; it can be set
  on the Setup tab. Client id `lora-gw-<last 6 hex of the EUI>`, clean
  session, keepalive 60 s.
- **Uplink JSON** (core, `lwc/ha.h`, host-tested): `node_id`, `name`,
  `dev_eui`, `dev_addr`, `fcnt`, `port`, `confirmed`, `payload_hex`, `rssi`,
  `snr`, `sf`, `bw_khz`, `freq_hz`, `ts` (UTC, only when the clock is set).
  There are no decoded sensor fields: the gateway has no payload codecs yet.
- **Discovery** (retained, QoS 1): a `binary_sensor` (connectivity) for the
  gateway, and RSSI and SNR sensors (diagnostic) for each local device, with
  `via_device` to the gateway, `availability_topic` = the status topic and
  `expire_after` (default 1 h, settable, 0 = off). All configs are sent again
  after each connect. Removing a device sends empty retained configs.
- **Stale configs are swept after each connect.** A removal while the broker
  is away, MQTT is off or across a restart cannot be published at the time.
  So for 10 s after each connect the gateway reads the retained
  `homeassistant/sensor/+/config` messages, and clears each one that points
  to this gateway (`via_device`) but names a DevEUI that is no longer a
  local device. Configs of other gateways and other integrations are left
  alone.
- **Only the local network server's own devices.** Events: accepted uplinks
  (`LWC_NS_UP_OK`, not retransmissions) and answered joins. Traffic forwarded
  to TTN in hybrid mode is not published.
- **Never block the radio path.** The `lns` task copies the event into a
  16-entry PSRAM queue with a zero timeout, after the downlink is sent. A
  separate `mqttha` task publishes. When MQTT is off, not connected or the
  queue is full, the event is dropped and counted. No backlog is kept while
  the broker is away (QoS 0, `expire_after` shows stale values).
- **Client:** `espressif/mqtt` 1.1.0 from the component registry (esp-mqtt
  left ESP-IDF in 6.0). Buffers 1536 B, outbox limit 32 KB in PSRAM,
  reconnect every 10 s. Library logging is off; the gateway logs each new
  failure once, so retries do not fill the log ring.
- **Settings** go through `/api/settings` (no new handlers) and are stored in
  NVS namespace `mqtt`. The password is write-only: never in the status, the
  logs or a URI.
- **Plain MQTT only.** The Setup tab says to use it on the local network.
  TLS and the design plan's central-broker mTLS stay open.

## Consequences

- Home Assistant shows the gateway and each device with no YAML.
- Sensor values (temperature and so on) need payload codecs; until then
  Home Assistant users can template `payload_hex` themselves.
- A second consumer that wants ChirpStack JSON needs another topic layout;
  `lwc/ha.h` keeps the JSON in core, so a second formatter is small.
- Changing the gateway id leaves the old gateway's status topic and gateway
  config on the broker (the sweep only knows the current id). Delete the old
  device in Home Assistant, or clear the two retained topics.

## Verification (2026-11-24, rev3 bench board, desk build, HA Mosquitto 7.1.3)

| Test | Result |
| --- | --- |
| Host tests (`test_ha`: ids, ISO 8601, topics, exact JSON, escaping, overflow, 242-byte payload) | pass |
| 8 bad settings (port 0, `a/b`, capitals, spaces, 70-character password, `mqtt=maybe`, long expiry, user with a space) | 400 each |
| Login refused by the broker (CONNACK 5) | state `refused`, one log line, password not in logs or status |
| User `a&b+c%d=e` through the form | read back unchanged |
| Connect | `online` retained, gateway and device configs retained |
| FiPy join + 3 uplinks (one confirmed) | `join` and `up` messages, fcnt 0-2, `confirmed` true on the third |
| Gateway held in the ROM bootloader | broker sent `offline` after ~88 s (last will) |
| Reboot | `online` and all configs again 19 s after reset |
| Session taken over (same client id) | `offline`, then `online` and all configs again 16 s later; `connects=2`; retained status `online` |
| Mosquitto app restarted in Home Assistant | reconnected 15 s after the drop, configs sent again, retained status `online`; no restart of the gateway (uptime 72 min) |
| Home Assistant, Settings > Devices > MQTT | "LoRa gateway ..." with Status, and the FiPy with RSSI and SNR |
| Stale config planted for this gateway, and one for another gateway | after reconnect the first is cleared, the second kept |
| Device added, then removed | announced after reboot (name with quotes escaped); empty retained configs on removal |
| Broker unreachable (wrong port), FiPy join + 3 uplinks | `dropped=4`; join answered and confirmed uplink acked |
| Stacks after the tests | `mqttha` 3012 of 4096 B free, `mqtt_task` 5076 of 6144 B free |

## Plan

1. Events out (this ADR).
2. Downlinks: `lora/<gateway_id>/<node_id>/down` into the local downlink
   queue.
3. Payload codecs, then decoded fields in the JSON and discovery.
