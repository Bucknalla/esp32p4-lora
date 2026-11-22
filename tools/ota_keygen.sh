#!/usr/bin/env bash
# Create the OTA signing key once (ADR-0008). It stays on this computer,
# outside the repo. The board accepts an OTA image only when it is signed by
# the key whose public part is in the image already running.
#
#   . ~/esp/v6.1.1/esp-idf/export.sh
#   tools/ota_keygen.sh            # key at ~/.config/esp32p4-lora/ota_signing_key.pem
#
# Back the key up. Lose it, and the next update must go over USB with a new key.
set -euo pipefail

key="${OTA_SIGNING_KEY:-$HOME/.config/esp32p4-lora/ota_signing_key.pem}"
if [ -e "$key" ]; then
    echo "key already exists: $key (not overwriting)" >&2
    exit 1
fi
mkdir -p "$(dirname "$key")"
chmod 700 "$(dirname "$key")"
umask 077
espsecure generate-signing-key --version 2 --scheme rsa3072 "$key"
chmod 600 "$key"
echo "created $key"
tmp="$(mktemp)"
espsecure digest-sbv2-public-key --keyfile "$key" --output "$tmp" >/dev/null
echo "public key digest (record it in your notes): $(xxd -p -c 64 "$tmp")"
rm -f "$tmp"
