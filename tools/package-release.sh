#!/usr/bin/env bash
# Collect a built tree into the release payload: the four flashable images, the merged image that
# ESP Web Tools needs, SHA256SUMS and the ESP Web Tools manifest.
#
#   tools/package-release.sh [VERSION] [OUTDIR]     # defaults: git describe, dist/
#
# Run after `idf.py build`. esptool in this toolchain is v5, whose subcommands are hyphenated
# (`merge-bin`, not `merge_bin`); @flash_args carries the offsets and flash settings the build
# actually used, so nothing here duplicates the partition layout.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

version="${1:-$(git describe --tags --dirty --always)}"
outdir="${2:-dist}"

[ -f build/esp_dali_gw.bin ] || { echo "build/esp_dali_gw.bin missing; run idf.py build" >&2; exit 1; }

echo "==> merging"
(cd build && esptool --chip esp32c6 merge-bin -o esp_dali_gw-merged.bin @flash_args)

rm -rf "$outdir"
mkdir -p "$outdir"
cp build/esp_dali_gw.bin \
   build/ota_data_initial.bin \
   build/esp_dali_gw-merged.bin \
   build/bootloader/bootloader.bin \
   build/partition_table/partition-table.bin \
   "$outdir/"

# ESP Web Tools flashes a single image at 0; the merged one already contains the bootloader, the
# partition table and the initial otadata at their offsets.
#
# improv_wait_time is 0 on purpose: provisioning is the device's own AP + captive portal (SPEC 5),
# not Improv Serial, so the default 10 s wait would only stall the page after a flash.
cat >"$outdir/manifest.json" <<JSON
{
  "name": "esp_dali_gw",
  "version": "$version",
  "funding_url": "https://calaos.fr/",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "esp_dali_gw-merged.bin", "offset": 0 }
      ]
    }
  ]
}
JSON

(cd "$outdir" && sha256sum ./*.bin manifest.json | sed 's| \./| |' >SHA256SUMS)

echo "==> $outdir"
ls -l "$outdir"
