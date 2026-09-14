#!/usr/bin/env bash
# Run every CI step locally, inside the same image CI uses, so a push cannot surprise you.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

run() { tools/docker-run.sh bash -c "$1"; }

echo "==> [1/4] web"
run 'npm --prefix web ci && npm --prefix web run lint && npm --prefix web run typecheck && npm --prefix web run build'
run 'tools/check-bundle-size.sh'

echo "==> [2/4] firmware"
run 'idf.py set-target esp32c6 && idf.py build'
run 'tools/check-app-size.sh'

echo "==> [3/4] host tests"
run 'tools/host-tests.sh'

echo "==> [4/4] lint"
run 'clang-format --dry-run --Werror $(git ls-files "*.c" "*.h")'
run 'cppcheck --enable=warning,portability --inline-suppr --std=c17 --suppress=missingInclude -I components/*/include components main' || true

echo "==> all CI steps passed"
