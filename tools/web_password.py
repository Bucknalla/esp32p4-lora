#!/usr/bin/env python3
"""Set a web UI password over the USB service console (ADR-0010).

    . ~/esp/v6.1.1/esp-idf/export.sh
    python tools/web_password.py <port> admin|viewer             # asks twice, no echo
    python tools/web_password.py <port> admin|viewer --generate  # random, saved to a file

--generate writes a random 20-character password to
~/.config/esp32p4-lora/web_<role>_password (mode 600) and does not print it.
The board stores only a salted PBKDF2-HMAC-SHA256 hash (NVS) and logs
everyone out. The password travels hex-encoded over the local USB link.
"""
import getpass
import os
import secrets
import string
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from provision_wifi import send  # noqa: E402

CONF = Path.home() / ".config/esp32p4-lora"


def main():
    args = [a for a in sys.argv[1:] if a != "--generate"]
    if len(args) != 2 or args[1] not in ("admin", "viewer"):
        sys.exit(__doc__)
    port, role = args
    if "--generate" in sys.argv[1:]:
        alphabet = string.ascii_letters + string.digits
        password = "".join(secrets.choice(alphabet) for _ in range(20))
        CONF.mkdir(mode=0o700, parents=True, exist_ok=True)
        path = CONF / f"web_{role}_password"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(password + "\n")
    else:
        path = None
        password = getpass.getpass(f"{role} password: ")
        if getpass.getpass("again: ") != password:
            sys.exit("passwords differ")
    data = password.encode("utf-8")
    if not 8 <= len(data) <= 63:
        sys.exit("password must be 8-63 bytes")
    reply = send(port, f"WEB_PASS {role} {data.hex()}")
    del password, data
    if reply is None:
        sys.exit("no reply from the board (is the gateway firmware running?)")
    if not reply.startswith("OK"):
        sys.exit(f"board replied: {reply}")
    print(f"board: {reply}" + (f"; password saved to {path}" if path else ""))


if __name__ == "__main__":
    main()
