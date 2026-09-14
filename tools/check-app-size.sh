#!/usr/bin/env bash
# Report the firmware size and fail when an OTA slot is more than 90% full (SPEC 15.2).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_DIR=${BUILD_DIR:-build}
BIN=$BUILD_DIR/esp_dali_gw.bin
[ -f "$BIN" ] || { echo "$BIN missing; run idf.py build"; exit 1; }

# Which partition table this build actually used: it differs per target (4 MB vs 16 MB boards), so
# reading partitions.csv unconditionally would check the image against the wrong slot.
table=$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\(.*\)"$/\1/p' \
        "$BUILD_DIR/../sdkconfig" 2>/dev/null)
[ -n "$table" ] && [ -f "$table" ] || table=partitions.csv

slot=$(awk -F, '/^ota_0/ { gsub(/ /, "", $5); print $5 }' "$table")
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
