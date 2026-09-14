#!/usr/bin/env bash
# Host unit tests for the pure-logic components.
#
# Plain CMake + Unity rather than the IDF linux target: gw_api deliberately depends on nothing but
# cJSON, and keeping this build IDF-free is what proves it. cJSON is fetched straight from the
# registry at the version pinned in dependencies.lock, so the test exercises the code that ships
# rather than whatever the distro packages -- and so this can run without configuring the firmware
# project, which would need a built web/dist.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

CJSON_DIR=managed_components/espressif__cjson

if [ ! -d "$CJSON_DIR" ]; then
    version=$(python3 -c '
import re, sys
text = open("dependencies.lock").read()
m = re.search(r"^  espressif/cjson:.*?^    version: (\S+)", text, re.S | re.M)
sys.stdout.write(m.group(1).strip("\"\x27") if m else "")
')
    [ -n "$version" ] || { echo "cjson version not found in dependencies.lock" >&2; exit 1; }

    echo "==> fetching espressif/cjson $version"
    # Ask the registry for the archive URL: versions carry revision suffixes ("1.7.19~2") that do
    # not map onto the file name by simple substitution.
    url=$(curl -fsSL "https://components.espressif.com/api/components/espressif/cjson/" \
          | python3 -c '
import json, sys
want = sys.argv[1]
data = json.load(sys.stdin)
sys.stdout.write(next(v["url"] for v in data["versions"] if v["version"] == want))
' "$version")
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    curl -fsSL "$url" -o "$tmp/cjson.zip"
    mkdir -p "$CJSON_DIR"
    unzip -oq "$tmp/cjson.zip" -d "$CJSON_DIR"
fi

cmake -S tests/host -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j"$(nproc)"
ctest --test-dir build-host --output-on-failure
