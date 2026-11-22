#!/usr/bin/env python3
"""Send Wi-Fi credentials to the board over its service console.

    . ~/esp/v6.1.1/esp-idf/export.sh
    python tools/provision_wifi.py <port> [ssid]
    python tools/provision_wifi.py <port> --clear

Asks for the password without echo (or reads WIFI_PASS from the environment).
The board stores the credentials in NVS and restarts. NVS is NOT encrypted
(ADR-0006 deferred): the password is plain text in the board's flash.
Nothing is written to disk on this computer. The values travel hex-encoded over
the local USB serial link; the board does not echo or log the password.

Protocol: core/include/lwc/console_cmd.h.
"""
import getpass
import os
import sys
import time

import serial

TIMEOUT_S = 10.0
BOOT_WAIT_S = 15.0


def _lines(s, end):
    buf = b""
    while time.time() < end:
        buf += s.read(256)
        *done, buf = buf.split(b"\n")
        for raw in done:
            yield raw.decode("utf-8", "replace").strip()
    yield None


def send(port, line):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    # macOS raises DTR and RTS on open. Keeping both on avoids the transient
    # RTS-on/DTR-off state that pulls EN low through the auto-reset circuit.
    s.dtr = True
    s.rts = True
    s.open()
    try:
        # If the board rebooted, wait until its console is listening.
        booting = False
        for text in _lines(s, time.time() + 1.5):
            if text is None:
                break
            if "rst:" in text or "ESP-ROM" in text:
                booting = True
                break
        if booting:
            for text in _lines(s, time.time() + BOOT_WAIT_S):
                if text is None:
                    return "no READY from the console after reset"
                if text == "CONSOLE: READY":
                    break
        # Flush any partial line (e.g. noise from the reset), drop its reply.
        s.write(b"\n")
        s.flush()
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(line.encode("ascii") + b"\n")
        s.flush()
        for text in _lines(s, time.time() + TIMEOUT_S):
            if text is None:
                return None
            if text.startswith("CONSOLE: ") and text != "CONSOLE: READY":
                return text[len("CONSOLE: "):]
        return None
    finally:
        s.close()


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    port = sys.argv[1]

    if len(sys.argv) == 3 and sys.argv[2] == "--clear":
        reply = send(port, "WIFI_CLEAR")
    else:
        ssid = sys.argv[2] if len(sys.argv) == 3 else input("SSID: ")
        password = os.environ.get("WIFI_PASS") or getpass.getpass("Password: ")
        s_bytes, p_bytes = ssid.encode("utf-8"), password.encode("utf-8")
        if not (1 <= len(s_bytes) <= 32) or not (8 <= len(p_bytes) <= 63):
            sys.exit("SSID must be 1-32 bytes and WPA2 password 8-63 bytes")
        reply = send(port, f"WIFI_SET {s_bytes.hex()} {p_bytes.hex()}")
        del password, p_bytes

    if reply is None:
        sys.exit("no reply from the board (is the gateway firmware running?)")
    if not reply.startswith("OK"):
        sys.exit(f"board replied: {reply}")
    print(f"board: {reply}; it restarts to apply the change")


if __name__ == "__main__":
    main()
