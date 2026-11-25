# ADR-0020: Class C for local devices

- Status: accepted (built and tested on the board)
- Date: 2026-11-25

## Context

Class A devices hear the network only right after their own uplinks, so an
actuator (a valve, a relay) waits up to its uplink interval for a command.
Class C devices keep their receiver on the RX2 channel between uplinks: the
network can reach them at any time. ADR-0018 queues application downlinks
per device and sends them with class A answers; class C needs them sent on
their own, with no uplink to answer.

## Decision

- **A class per device, A or C**, set when the device is added
  (`class=A|C`) or later (`POST /api/devices/settings`, the Devices tab);
  the session is kept. It is in the device record (version 3; version 2 records
  are migrated as class A). Firmware before this change does not read
  version 3 records: after a rollback the devices are skipped until they are
  added again.
- **When a class C device has a downlink waiting**, the gateway sends it at
  once on the device's RX2 frequency and data rate (from its join-accept), at
  the concentrator counter + 300 ms. The frame has no MAC commands and no
  ACK bit; FPending when more wait; Confirmed Data Down when asked. It must
  fit the RX2 data rate (a larger one is refused when published).
- **Only after the session is proven** (an uplink since the join), and not
  while the device's own class A windows are open (from its uplink to RX2 +
  1.5 s): then the head goes in the class A answer as before.
- **Pace: one send every 5 s per device at most.** Each class C send is a
  transmission chosen by whoever can publish on the broker: the receiver is
  deaf for its airtime, it spends the 869.4-869.65 MHz duty-cycle budget
  (10 %) that class A RX2 answers and the Basics Station link also use (the
  radio task keeps one budget per sub-band for every transmission), and it
  writes the downlink counter to NVS once. At DR0 a 51-byte frame is about
  2.5 s on air, so the duty cycle allows one such frame every 25 s; the gap
  keeps short commands quick without letting one device take the band.
- **Settling** (rules in core, `lwc/dlq`, host-tested):
  - the radio's answer is kept per device (the queue head is the only frame
    of that device on its way);
  - handed to the concentrator: done for an unconfirmed downlink (`sent`);
  - refused by the radio (duty cycle, busy): tried again after 5, 10, 15,
    20 s, dropped after 5 refusals;
  - no answer from the radio within 10 s (a radio restart): dropped, never
    sent again, since it may have been on air;
  - confirmed: `acked` by a new uplink with FCtrl.ACK received after the
    downlink ended (an uplink the device sent before it could hear the
    downlink settles nothing); `nacked` by a later uplink without it; no
    uplink within the device's ACK wait: `nacked` with the reason "no uplink
    with the ACK in time". Never resent on its own (ADR-0018).
- **The ACK wait is per device**: `c_ack_s` 5-3600 s, default 60 s (0 in
  the API returns to it), in the device record and on the Devices tab.
- **Local devices only.** Class C downlinks from TTN that answer no uplink
  are still refused (ADR-0016: no timed transmit without GPS time).
- The `lns` task wakes every 100 ms (was 1 s) to serve class C queues.

## Consequences

- A class C device gets a command within about 0.4 s of its publication
  (plus up to 5 s behind another one).
- The default 60 s ACK wait suits devices that answer a confirmed downlink
  with an uplink soon; for a device that waits for its next periodic uplink,
  set its ACK wait above its uplink interval, or it gets `nacked` (timeout)
  although it may act on the command.
- A class C device that did not switch its radio to class C misses these
  downlinks: the gateway cannot tell (LoRaWAN 1.0.x has no DeviceModeInd).

## Verification (2026-11-25, rev3 bench board, desk build, 0 dBm cap)

| Test | Result |
| --- | --- |
| Host tests (`test_dlq`: send, gap, delivered on the radio's answer, an uplink before the downlink ended ignored and not offered, counter wrap, ACK, timeout with its reason, refusal pauses and the drop after 5, no answer dropped, hold for the class A windows, defer; `test_ns`: class C frame read as the device reads it, confirmed with FPending, RX2 size limit, no proven session, class setting, timed RX2 transmit) | pass, with ASan/UBSan |
| Firmware with a version 2 record (the FiPy, joined) | "record migrated to version 3"; same fields, class A; an uplink on the existing session accepted (FCnt 4, no new join) |
| FiPy joined as class C, one uplink; then `cc1` (unconfirmed, port 31) and `cc2` (confirmed, port 32) published | FiPy received `a1` 2 s after its uplink and `b2` 5 s later, no uplink between; results: cc1 queued, sent; cc2 queued, sent (5 s later), acked at the FiPy's next uplink 55 s later. Two class C frames on air (14 B each, DR0) |
| Settings API: `c_ack_s` 4, 3601, `x`, class B, nothing given; then 10, 0, class A with 120; an unknown DevEUI | 400 each, nothing changed; then saved (10, back to 60, 120); 404 |
| FiPy as class C with an ACK wait of 10 s; confirmed `cc3` published, the FiPy's next uplink 50 s later | received 1 s after its first uplink; results: queued, sent, then `nacked` 10 s later, "no uplink with the ACK in time" |
| Class A again (FiPy reset in between): confirmed + unconfirmed downlink, 4 uplinks | received in order; c1 queued, sent, acked; u1 queued, sent; 4 uplinks published |
| Stacks after the tests | `lns` 4200, `radio` 7060, `mqttc` 2916, `mqtt_task` 4904 B free |

The FiPy (Pycom firmware) crashes when its LoRa stack is switched from class
C back to class A without a reset; `machine.reset()` between the two avoids
it. This is the test device, not the gateway.
