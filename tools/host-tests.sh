#!/usr/bin/env bash
# Host unit tests for the two components that are pure logic: gw_api and app_config.
# Plain CMake + Unity rather than the IDF linux target, which drags in too much of the IDF for
# app_config to be testable without NVS mocks.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Managed components are fetched by the IDF, not vendored; the host build needs the same cJSON
# that ships on the device.
if [ ! -d managed_components/espressif__cjson ]; then
    echo "==> fetching managed components"
    idf.py reconfigure >/dev/null
fi

cmake -S tests/host -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j"$(nproc)"
ctest --test-dir build-host --output-on-failure
