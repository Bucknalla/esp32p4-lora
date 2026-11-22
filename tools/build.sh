#!/usr/bin/env bash
# Build the firmware for one chip revision, each in its own build directory.
#   tools/build.sh rev3 [idf.py args...]     e.g. tools/build.sh rev3 -p /dev/cu.usbmodemX flash
#   tools/build.sh rev1
# Needs ESP-IDF exported (. $IDF_PATH/export.sh) at the version in .esp-idf-version.
#
# Signing (ADR-0008): when the OTA signing key exists, the app is signed during
# the build. The key never lives in the repo:
#   OTA_SIGNING_KEY (default ~/.config/esp32p4-lora/ota_signing_key.pem)
# Create it once with tools/ota_keygen.sh. Without it the build is unsigned:
# fine for CI, but an unsigned image on the board refuses every OTA.
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

build_dir="${GW_BUILD_DIR:-$root/build/fw-$rev}"
mkdir -p "$build_dir"

key="${OTA_SIGNING_KEY:-$HOME/.config/esp32p4-lora/ota_signing_key.pem}"
frag="$build_dir/sdkconfig.signing"
if [ -f "$key" ]; then
    printf 'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y\nCONFIG_SECURE_BOOT_SIGNING_KEY="%s"\n' "$key" >"$frag"
else
    printf '# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set\n' >"$frag"
    echo "warning: no signing key at $key: building UNSIGNED (cannot accept OTA)" >&2
fi
# Dev builds (default): no automatic install of the latest release (a USB-
# flashed build would be replaced within a minute) and console test commands.
# Release builds (GW_RELEASE=1, tools/ota_release.sh): the opposite.
if [ "${GW_RELEASE:-0}" = 1 ]; then
    printf 'CONFIG_GW_OTA_AUTO_INSTALL=y\n# CONFIG_GW_TEST_COMMANDS is not set\n' >>"$frag"
else
    printf '# CONFIG_GW_OTA_AUTO_INSTALL is not set\nCONFIG_GW_TEST_COMMANDS=y\n' >>"$frag"
fi
# An existing sdkconfig wins over defaults: start it again if the fragment changed.
if [ -f "$build_dir/sdkconfig" ]; then
    while IFS= read -r line; do
        if ! grep -qxF "$line" "$build_dir/sdkconfig"; then
            echo "signing/release setting changed: regenerating $build_dir/sdkconfig" >&2
            rm -f "$build_dir/sdkconfig"
            break
        fi
    done <"$frag"
fi

args=("$@")
[ ${#args[@]} -eq 0 ] && args=(build)

exec idf.py -C "$root/firmware" -B "$build_dir" \
    -DIDF_TARGET=esp32p4 \
    -DSDKCONFIG="$build_dir/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$rev;$frag" \
    "${args[@]}"
