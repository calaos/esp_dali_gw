#!/usr/bin/env bash
# Collect one target's built tree into the release payload: the four flashable images, the merged
# image that ESP Web Tools needs, that target's manifest and its share of the checksums.
#
#   tools/package-release.sh TARGET [VERSION] [OUTDIR]
#   tools/package-release.sh esp32c6                    # version from git describe, into dist/
#
# Run after `idf.py -B build-TARGET build`. Every file is suffixed with the target because the
# release is one flat namespace and `bootloader.bin` means nothing on its own there.
#
# Checksums land in SHA256SUMS.TARGET, never SHA256SUMS: the two targets are packaged by two
# separate CI jobs and a single name would have them overwrite each other. Whoever collects both
# payloads concatenates the parts (see the publish job in .github/workflows/release.yml).
#
# esptool in this toolchain is v5, whose subcommands are hyphenated (`merge-bin`, not `merge_bin`);
# @flash_args carries the offsets and flash settings the build actually used, so nothing here
# duplicates the partition layout — which differs between the 4 MB C6 and the 16 MB S3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

target="${1:?usage: package-release.sh TARGET [VERSION] [OUTDIR]}"
version="${2:-$(git describe --tags --dirty --always)}"
outdir="${3:-dist}"
builddir="${BUILD_DIR:-build-$target}"

# An unknown target would otherwise reach the manifest as a chipFamily ESP Web Tools rejects at
# flash time, on the user's desk, rather than here.
case "$target" in
esp32c6) chip_family="ESP32-C6"; board="ESP32-C6-Pico" ;;
esp32s3) chip_family="ESP32-S3"; board="ESP32-S3-Pico" ;;
*) echo "unknown target: $target" >&2; exit 1 ;;
esac

[ -f "$builddir/esp_dali_gw.bin" ] || {
    echo "$builddir/esp_dali_gw.bin missing; run idf.py -B $builddir build" >&2
    exit 1
}

merged="esp_dali_gw-merged-$target.bin"

echo "==> merging $target"
(cd "$builddir" && esptool --chip "$target" merge-bin -o "$merged" @flash_args)

mkdir -p "$outdir"
cp "$builddir/esp_dali_gw.bin"                        "$outdir/esp_dali_gw-$target.bin"
cp "$builddir/ota_data_initial.bin"                   "$outdir/ota_data_initial-$target.bin"
cp "$builddir/$merged"                                "$outdir/$merged"
cp "$builddir/bootloader/bootloader.bin"              "$outdir/bootloader-$target.bin"
cp "$builddir/partition_table/partition-table.bin"    "$outdir/partition-table-$target.bin"

# ESP Web Tools flashes a single image at 0; the merged one already contains the bootloader, the
# partition table and the initial otadata at their offsets.
#
# The board is in the name because that string is what the install dialog shows the user, and the
# two Pico boards are physically indistinguishable.
#
# improv_wait_time is 0 on purpose: provisioning is the device's own AP + captive portal (SPEC 5),
# not Improv Serial, so the default 10 s wait would only stall the page after a flash.
cat >"$outdir/manifest-$target.json" <<JSON
{
  "name": "esp_dali_gw ($board)",
  "version": "$version",
  "funding_url": "https://calaos.fr/",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "$chip_family",
      "parts": [
        { "path": "$merged", "offset": 0 }
      ]
    }
  ]
}
JSON

(cd "$outdir" && sha256sum ./*-"$target".bin "manifest-$target.json" | sed 's| \./| |' \
    >"SHA256SUMS.$target")

echo "==> $outdir ($target)"
ls -l "$outdir"/*"$target"*
