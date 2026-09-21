#!/usr/bin/env bash
set -euo pipefail

# Verifies the platform-independent Swift core against the C++ engine using the
# official Swift Linux image.
#
# This is not a macOS simulator: AppKit and InputMethodKit cannot run in a
# Linux container. It does compile and execute the parts that talk to the
# engine (C ABI bridge, callbacks, key translation, render snapshots), which is
# the integration surface the macOS app is built on.
#
# Requires Docker. Usage: macos/scripts/verify-core-linux.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${LLAVON_IME_SWIFT_VERIFY_IMAGE:-llavon-ime-swift-verify}"

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required for this verification." >&2
    exit 2
fi

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "Building ${IMAGE} (first run only)..."
    docker build -t "${IMAGE}" -f "${ROOT_DIR}/macos/scripts/Dockerfile.verify" "${ROOT_DIR}/macos/scripts"
fi

docker run --rm \
    -v "${ROOT_DIR}:/work:ro" \
    -w /work \
    -e "LLAVON_IME_CORE_VERIFY_BUILD_DIR=/tmp/verify-core" \
    "${IMAGE}" \
    bash macos/scripts/verify-core.sh
