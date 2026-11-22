#!/usr/bin/env bash
# Build, sign and publish a firmware release (ADR-0008).
#
#   . ~/esp/v6.1.1/esp-idf/export.sh
#   tools/ota_release.sh v0.2.0            # needs a clean working tree
#   tools/ota_release.sh v0.2.0 --allow-dirty
#   tools/ota_release.sh v0.2.0 --draft    # upload, but gateways do not see it yet
#
# Builds rev3 and rev1 from scratch with PROJECT_VER=<version>, checks each
# image's signature with the local key, and creates a GitHub release (tag on
# the current commit, which must be pushed) in the source repo with lorawan_gateway-rev3.bin and -rev1.bin. Gateways
# pull ".../releases/latest/download/lorawan_gateway-<rev>.bin".
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
repo="${GW_FW_REPO:-Bucknalla/esp32p4-lora}"
key="${OTA_SIGNING_KEY:-$HOME/.config/esp32p4-lora/ota_signing_key.pem}"

version="${1:-}"
shift || true
allow_dirty=0
draft=()
for a in "$@"; do
    case "$a" in
        --allow-dirty) allow_dirty=1 ;;
        --draft) draft=(--draft) ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
if ! [[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.]+)?$ ]] || [ ${#version} -gt 31 ]; then
    echo "usage: $0 vMAJOR.MINOR.PATCH[-suffix] [--allow-dirty] [--draft]  (max 31 chars)" >&2
    exit 2
fi
[ -f "$key" ] || { echo "no signing key at $key (tools/ota_keygen.sh)" >&2; exit 1; }

head="$(git -C "$root" rev-parse HEAD)"
commit="$(git -C "$root" rev-parse --short HEAD)"
if ! git -C "$root" branch -r --contains "$head" | grep -q .; then
    echo "commit $commit is not pushed: push it first (the release tag points at it)" >&2
    exit 1
fi
if [ -n "$(git -C "$root" status --porcelain)" ]; then
    if [ "$allow_dirty" -ne 1 ]; then
        echo "working tree not clean: commit first, or pass --allow-dirty" >&2
        exit 1
    fi
    commit="$commit-dirty"
fi

dist="$root/build/release-$version"
rm -rf "$dist"
mkdir -p "$dist"
for rev in rev3 rev1; do
    bdir="$root/build/rel-$rev"
    rm -rf "$bdir"
    GW_FW_VERSION="$version" GW_BUILD_DIR="$bdir" "$root/tools/build.sh" "$rev" build >"$dist/build-$rev.log" 2>&1 ||
        { echo "build $rev failed: see $dist/build-$rev.log" >&2; exit 1; }
    espsecure verify-signature --version 2 --keyfile "$key" "$bdir/lorawan_gateway.bin" >/dev/null ||
        { echo "$rev image signature does not verify" >&2; exit 1; }
    cp "$bdir/lorawan_gateway.bin" "$dist/lorawan_gateway-$rev.bin"
done
(cd "$dist" && shasum -a 256 lorawan_gateway-*.bin >SHA256SUMS)

notes="Source: $commit. ESP-IDF v6.1.1. Signed (Secure Boot v2 RSA-3072, no secure boot).

\`\`\`
$(cat "$dist/SHA256SUMS")
\`\`\`"
gh release create "$version" --repo "$repo" --target "$head" --title "$version" --notes "$notes" "${draft[@]}" \
    "$dist/lorawan_gateway-rev3.bin" "$dist/lorawan_gateway-rev1.bin" "$dist/SHA256SUMS"
echo "published $version ($commit) to $repo"
