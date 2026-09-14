#!/usr/bin/env bash
# Runs once after the devcontainer is created. Idempotent.
set -euo pipefail

cd "$(dirname "$0")/.."

echo "==> Toolchain (baked into the image by .devcontainer/Dockerfile)"
idf.py --version
node --version
clang-format --version

echo "==> Web UI deps"
if [ -f web/package.json ]; then
  (cd web && npm ci)
fi

echo "==> Pre-fetch managed components so the first build works offline"
if [ -f main/idf_component.yml ]; then
  idf.py reconfigure >/dev/null 2>&1 || true
fi

echo "==> Serial devices visible in the container:"
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "   (none — plug the board or see .devcontainer/devcontainer.json for macOS/Windows notes)"

echo "==> Done. Build with: npm --prefix web run build && idf.py build"
