#!/usr/bin/env bash
# Fail if the OS-independent core library includes ESP-IDF or FreeRTOS headers.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
pattern='#[[:space:]]*include[[:space:]]*[<"](esp_|freertos/|driver/|hal/|soc/|sdkconfig|rom/|nvs|lwip/)'
if grep -rnE "$pattern" "$root/core"; then
    echo "error: core/ must not include ESP-IDF or FreeRTOS headers" >&2
    exit 1
fi
echo "core include check: OK"
