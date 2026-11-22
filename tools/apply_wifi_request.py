#!/usr/bin/env python3
"""Apply a Wi-Fi request saved from the "Gateway Wi-Fi Setup" page, then delete it.

    python tools/apply_wifi_request.py <port> <request.json>

<request.json> is the page's `requests/wifi` document as saved to disk (a JSON
object with a "data" object holding "ssid" and "pass", or those fields at top
level). The password is read from the file, sent over the service console
(tools/provision_wifi.py protocol) and never printed. The file is deleted
afterwards, whether the board accepted it or not.
"""
import json
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from provision_wifi import send  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port, path = sys.argv[1], pathlib.Path(sys.argv[2])
    try:
        doc = json.loads(path.read_text())
        body = doc.get("data", doc) if isinstance(doc, dict) else {}
        ssid, password = body.get("ssid"), body.get("pass")
        if not isinstance(ssid, str) or not isinstance(password, str):
            sys.exit("request has no ssid/pass strings")
        s_b, p_b = ssid.encode("utf-8"), password.encode("utf-8")
        if not (1 <= len(s_b) <= 32) or not (8 <= len(p_b) <= 63):
            sys.exit("request out of range: SSID 1-32 bytes, password 8-63 bytes")
        reply = send(port, f"WIFI_SET {s_b.hex()} {p_b.hex()}")
        del password, p_b
    finally:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    print(json.dumps({"ssid": ssid, "reply": reply}))
    sys.exit(0 if reply and reply.startswith("OK") else 1)


if __name__ == "__main__":
    main()
