#!/usr/bin/env bash
# Report the firmware size and fail when an OTA slot is more than 90% full (SPEC 15.2).
#
#   tools/check-app-size.sh [BUILD_DIR]     # default: build
#
# The slot size comes from the partition table that this build actually used, found through the
# build directory's own config — the C6 and the S3 do not share a partition table and must not
# share a budget.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

build="${1:-build}"
BIN="$build/esp_dali_gw.bin"
[ -f "$BIN" ] || { echo "$BIN missing; run idf.py -B $build build"; exit 1; }

csv=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["PARTITION_TABLE_CUSTOM_FILENAME"])' \
    "$build/config/sdkconfig.json")
slot=$(awk -F, '/^ota_0/ { gsub(/ /, "", $5); print $5 }' "$csv")
slot=$((slot))
used=$(stat -c%s "$BIN")
pct=$((used * 100 / slot))

# Reconfiguring under a different sdkconfig would silently rewrite this build, so reuse the one
# the build directory was created with.
sdkconfig=$(sed -n 's/^SDKCONFIG:[^=]*=//p' "$build/CMakeCache.txt")
idf.py -B "$build" ${sdkconfig:+-D SDKCONFIG="$sdkconfig"} size || true

echo
printf 'build     : %s (%s)\n' "$build" "$csv"
printf 'app image : %d bytes\n' "$used"
printf 'ota slot  : %d bytes\n' "$slot"
printf 'used      : %d%%\n' "$pct"
printf 'free      : %d bytes\n' $((slot - used))

if [ "$pct" -gt 90 ]; then
    echo "FAIL: OTA slot more than 90% full" >&2
    exit 1
fi
