#!/usr/bin/env bash
# Report the firmware size and fail when an OTA slot is more than 90% full (SPEC 15.2).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN=build/esp_dali_gw.bin
[ -f "$BIN" ] || { echo "$BIN missing; run idf.py build"; exit 1; }

# Slot size comes from the partition table so the two can never drift apart.
slot=$(awk -F, '/^ota_0/ { gsub(/ /, "", $5); print $5 }' partitions.csv)
slot=$((slot))
used=$(stat -c%s "$BIN")
pct=$((used * 100 / slot))

idf.py size || true

echo
printf 'app image : %d bytes\n' "$used"
printf 'ota slot  : %d bytes\n' "$slot"
printf 'used      : %d%%\n' "$pct"
printf 'free      : %d bytes\n' $((slot - used))

if [ "$pct" -gt 90 ]; then
    echo "FAIL: OTA slot more than 90% full" >&2
    exit 1
fi
