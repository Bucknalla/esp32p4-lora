#!/usr/bin/env bash
# Build the ESP32-C6 co-processor firmware (c6-firmware/) and write it into the
# P4's `cp_fw` partition. On the next boot, c6_update.c pushes it to the C6 if
# the C6's ESP-Hosted version differs from the host's.
#   tools/flash_c6_image.sh <port>
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
port="${1:?usage: $0 <port>}"
: "${IDF_PATH:?IDF_PATH not set: source export.sh}"

idf.py -C "$root/c6-firmware" -B "$root/build/c6" -DIDF_TARGET=esp32c6 \
    -DSDKCONFIG="$root/build/c6/sdkconfig" build
bin="$root/build/c6/lorawan_gw_c6.bin"

read -r offset size < <(python3 - "$root/firmware/partitions.csv" <<'PY'
import csv, sys
for row in csv.reader(l for l in open(sys.argv[1]) if not l.lstrip().startswith("#")):
    c = [x.strip() for x in row]
    if len(c) >= 5 and c[0] == "cp_fw":
        print(c[3], c[4]); break
else:
    sys.exit("cp_fw partition not found")
PY
)
bytes=$(wc -c < "$bin")
if [ "$bytes" -gt $((size)) ]; then
    echo "error: C6 image ($bytes bytes) larger than cp_fw ($size)" >&2; exit 1
fi
python -m esptool --chip esp32p4 -p "$port" -b 921600 write-flash "$offset" "$bin"
echo "C6 image ($bytes bytes) written to cp_fw at $offset"
