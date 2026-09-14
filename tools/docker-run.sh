#!/usr/bin/env bash
# Run a command inside the toolchain image (the same image the devcontainer and CI use).
#
#   tools/docker-run.sh idf.py build
#   tools/docker-run.sh npm --prefix web ci
#
# Building the image is handled here so a fresh clone needs nothing but Docker. The workspace is
# bind-mounted at the same path the devcontainer uses so build/ artefacts carry absolute paths that
# stay valid across both.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${TOOLCHAIN_IMAGE:-esp_dali_gw-toolchain:local}"

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "==> building $image" >&2
    docker build -f "$repo_root/.devcontainer/Dockerfile" \
        --build-arg INSTALL_CLAUDE_CODE=0 -t "$image" "$repo_root" >&2
fi

exec docker run --rm \
    -v "$repo_root:/workspaces/esp_dali_gw" \
    -v esp_dali_gw-ccache:/home/dev/.cache/ccache \
    -w /workspaces/esp_dali_gw \
    -u "$(id -u):$(id -g)" \
    -e HOME=/home/dev \
    --entrypoint bash \
    "$image" \
    -c 'exec "$0" "$@"' "$@"
