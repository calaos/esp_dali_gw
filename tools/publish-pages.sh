#!/usr/bin/env bash
# Add one release to a gh-pages working tree, in place.
#
#   tools/publish-pages.sh v1.0.0 dist path/to/gh-pages-checkout
#
# Only ever writes index.html, .nojekyll, versions.json and <version>/<target>/ — anything else
# already on the branch is left alone. Re-running the same version overwrites those directories and
# produces an identical versions.json, so a re-run of a release is a no-op.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

version="${1:?usage: publish-pages.sh VERSION DIST PAGES}"
dist="${2:?}"
pages="${3:?}"

[[ "$version" =~ ^v[0-9A-Za-z._+-]+$ ]] || { echo "refusing odd version: $version" >&2; exit 1; }

# Which targets this release has is whatever the payload contains, not a list kept in step with it.
shopt -s nullglob
manifests=("$dist"/manifest-*.json)
[ ${#manifests[@]} -gt 0 ] || { echo "no manifest-*.json in $dist" >&2; exit 1; }

for m in "${manifests[@]}"; do
    target="${m##*/manifest-}"
    target="${target%.json}"
    mkdir -p "$pages/$version/$target"
    cp "$m" "$pages/$version/$target/manifest.json"
    cp "$dist/esp_dali_gw-merged-$target.bin" "$pages/$version/$target/"
done

cp tools/webflash/index.html "$pages/index.html"

# GitHub Pages runs Jekyll by default, which would swallow anything it does not recognise.
touch "$pages/.nojekyll"

chip_to_target() {
    case "$1" in
    ESP32-C6) echo esp32c6 ;;
    ESP32-S3) echo esp32s3 ;;
    *) echo "" ;;
    esac
}

# Newest first, from what is actually on the branch — so the index survives a manual deletion and
# never lists a version whose files are gone.
#
# v0.1.0 predates the per-board directories and keeps its manifest at the top of its version
# directory. That layout is read here rather than rewritten, so an already-published release is
# never moved out from under a link someone has. A per-board directory wins over the flat manifest
# if both are present, which is what re-cutting such a version would leave behind.
entries=()
while IFS= read -r v; do
    [[ "$v" =~ ^v[0-9A-Za-z._+-]+$ ]] || continue
    builds=()
    for d in "$pages/$v"/*/; do
        t="$(basename "$d")"
        [ -f "$d/manifest.json" ] || continue
        builds+=("      { \"target\": \"$t\", \"manifest\": \"$v/$t/manifest.json\" }")
    done
    if [ ${#builds[@]} -eq 0 ] && [ -f "$pages/$v/manifest.json" ]; then
        chip=$(sed -n 's/.*"chipFamily"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            "$pages/$v/manifest.json" | head -n1)
        t="$(chip_to_target "$chip")"
        if [ -n "$t" ]; then
            builds+=("      { \"target\": \"$t\", \"manifest\": \"$v/manifest.json\" }")
        else
            echo "warning: $v has a manifest for unknown chipFamily '$chip'; skipping" >&2
        fi
    fi
    [ ${#builds[@]} -gt 0 ] || continue
    entries+=("  {
    \"version\": \"$v\",
    \"builds\": [
$(printf '%s,\n' "${builds[@]}" | sed '$s/,$//')
    ]
  }")
done < <(find "$pages" -maxdepth 1 -mindepth 1 -type d -name 'v*' -printf '%f\n' | sort -Vr)

{
    echo '['
    [ ${#entries[@]} -gt 0 ] && printf '%s,\n' "${entries[@]}" | sed '$s/,$//'
    echo ']'
} >"$pages/versions.json"

echo "==> $pages"
cat "$pages/versions.json"
