#!/usr/bin/env bash
# One-command cross build via Docker. Nothing needs to be installed locally
# except Docker itself.
#
#   ./build-docker.sh              # windows-x64 + linux-arm64 (the common pair)
#   ./build-docker.sh all          # every target below
#   ./build-docker.sh linux-arm64  # just one
#
# Targets: windows-x64 windows-x86 windows-arm64 windows-arm
#          linux-x64   linux-x86   linux-arm64   linux-arm
#
# Artifacts land in ./dist, or in $DIST_DIR if set (useful when a server is
# still running out of ./dist and holding the files open).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=betterserver-build
OUTDIR="${DIST_DIR:-dist}"

echo "[*] Building the toolchain image (cached after the first run)..."
docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile" "$ROOT/docker"

mkdir -p "$ROOT/$OUTDIR"

echo "[*] Cross-compiling..."
docker run --rm \
    -v "$ROOT:/src:ro" \
    -v "$ROOT/$OUTDIR:/out" \
    -e BUILD_TYPE="${BUILD_TYPE:-Release}" \
    "$IMAGE" bash /src/docker/build.sh "$@"
