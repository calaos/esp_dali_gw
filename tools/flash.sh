#!/usr/bin/env bash
# Flash a built image from the HOST, for macOS and Windows where Docker Desktop cannot pass the USB
# serial port into the container. Build in the container, flash with this.
#
#   tools/flash.sh                 # auto-detect the port
#   tools/flash.sh /dev/ttyACM0
#
# Needs esptool on the host (pipx install esptool). On Linux prefer `idf.py -p ... flash` inside the
# devcontainer, which already has everything.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

port="${1:-${ESPPORT:-}}"
merged=build/esp_dali_gw-merged.bin

command -v esptool >/dev/null || { echo "esptool not found on the host" >&2; exit 1; }
[ -d build ] || { echo "build/ missing; run idf.py build in the container first" >&2; exit 1; }

if [ ! -f "$merged" ]; then
    echo "==> creating $merged"
    (cd build && esptool --chip esp32c6 merge-bin -o "$(basename "$merged")" @flash_args)
fi

args=(--chip esp32c6)
[ -n "$port" ] && args+=(--port "$port")

# The merged image already carries the bootloader, partition table and ota_data at their offsets.
exec esptool "${args[@]}" --baud "${ESPBAUD:-921600}" write-flash 0x0 "$merged"
