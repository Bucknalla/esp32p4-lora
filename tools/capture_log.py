#!/usr/bin/env python3
"""Print the board's serial log for N seconds (no TTY needed).

    python tools/capture_log.py <port> [seconds]              # reset first, log from boot
    python tools/capture_log.py <port> [seconds] --no-reset   # watch the running board

macOS raises DTR and RTS when it opens the port. The auto-reset circuit pulls
EN low only for RTS on + DTR off, so the port is opened with both on (no
reset), and a reset, when wanted, is an explicit RTS pulse with DTR off.

Retries once when nothing arrives: right after esptool's own reset the USB
bridge can be busy, and an empty capture must not be mistaken for "no boot".
Exits 1 if both attempts read nothing.
"""
import sys
import time

import serial


def capture(port, secs, reset):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True  # both on at open: no reset glitch
    s.rts = True
    s.open()
    s.reset_input_buffer()
    if reset:
        s.dtr = False  # RTS on + DTR off: EN low, boot strap high
        time.sleep(0.1)
        s.dtr = True   # both on again: EN released, normal boot
    got = 0
    end = time.time() + secs
    while time.time() < end:
        data = s.read(4096)
        if data:
            got += len(data)
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()
    return got


def main():
    args = [a for a in sys.argv[1:] if a != "--no-reset"]
    reset = "--no-reset" not in sys.argv
    port = args[0]
    secs = float(args[1]) if len(args) > 1 else 20.0
    for attempt in (1, 2):
        if capture(port, secs, reset) > 0:
            return 0
        sys.stderr.write(f"capture_log: no data on attempt {attempt}\n")
        time.sleep(1.0)
    return 1


if __name__ == "__main__":
    sys.exit(main())
