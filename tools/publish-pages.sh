#!/usr/bin/env bash
# Add one release to a gh-pages working tree, in place.
#
#   tools/publish-pages.sh v1.0.0 dist path/to/gh-pages-checkout
#
# Only ever writes index.html, .nojekyll, versions.json and <version>/ — anything else already on
# the branch is left alone. Re-running the same version overwrites that one directory and produces
# an identical versions.json, so a re-run of a release is a no-op.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

version="${1:?usage: publish-pages.sh VERSION DIST PAGES}"
dist="${2:?}"
pages="${3:?}"

[[ "$version" =~ ^v[0-9A-Za-z._+-]+$ ]] || { echo "refusing odd version: $version" >&2; exit 1; }

mkdir -p "$pages/$version"
cp "$dist/manifest.json" "$dist/esp_dali_gw-merged.bin" "$pages/$version/"
cp tools/webflash/index.html "$pages/index.html"

# GitHub Pages runs Jekyll by default, which would swallow anything it does not recognise.
touch "$pages/.nojekyll"

# Newest first, from what is actually on the branch — so the index survives a manual deletion and
# never lists a version whose files are gone.
{
    echo '['
    find "$pages" -maxdepth 1 -mindepth 1 -type d -name 'v*' -printf '%f\n' \
        | sort -Vr \
        | sed 's/.*/  "&",/' \
        | sed '$s/,$//'
    echo ']'
} >"$pages/versions.json"

echo "==> $pages"
cat "$pages/versions.json"
