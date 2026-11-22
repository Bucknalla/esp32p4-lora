#!/usr/bin/env bash
# Build the firmware for one chip revision, each in its own build directory.
#   tools/build.sh rev3 [idf.py args...]     e.g. tools/build.sh rev3 -p /dev/cu.usbmodemX flash
#   tools/build.sh rev1
# Needs ESP-IDF exported (. $IDF_PATH/export.sh) at the version in .esp-idf-version.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
rev="${1:-}"
case "$rev" in
    rev3|rev1) shift ;;
    *) echo "usage: $0 rev3|rev1 [idf.py args...]" >&2; exit 2 ;;
esac

want="$(cat "$root/.esp-idf-version")"
: "${IDF_PATH:?IDF_PATH not set: source export.sh}"
have="$(idf.py --version 2>/dev/null | sed -n 's/^ESP-IDF \(v[0-9.]*\).*/\1/p')"
if [ "$have" != "$want" ]; then
    echo "error: ESP-IDF $want required, IDF_PATH is at $have" >&2
    exit 1
fi

build_dir="$root/build/fw-$rev"
args=("$@")
[ ${#args[@]} -eq 0 ] && args=(build)

exec idf.py -C "$root/firmware" -B "$build_dir" \
    -DIDF_TARGET=esp32p4 \
    -DSDKCONFIG="$build_dir/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$rev" \
    "${args[@]}"
