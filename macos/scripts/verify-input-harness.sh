#!/usr/bin/env bash
set -euo pipefail

# Compiles and runs the raw-input harness for the native macOS frontend: real
# NSEvents through LlavonInputController, asserting the client-side effects
# (marked text, commits, passthrough). macOS only; the fcitx5 addon has its own
# harness under fcitx5/tests driven by fcitx5's TestFrontend.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${LLAVON_IME_INPUT_HARNESS_BUILD_DIR:-${ROOT_DIR}/build/verify-input-harness}"
ENGINE_BUILD_DIR="${BUILD_DIR}/engine"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(command -v ninja)")
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

cmake -S "${ROOT_DIR}/engine" -B "${ENGINE_BUILD_DIR}" \
    ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DLLAVON_IME_ENGINE_BUILD_TESTS=OFF
cmake --build "${ENGINE_BUILD_DIR}" --target llavon_ime_engine --parallel

swiftc -O -parse-as-library \
    -module-name LlavonIMEInputHarness \
    -I "${ROOT_DIR}/engine/include" \
    "${ROOT_DIR}"/macos/Core/*.swift \
    "${ROOT_DIR}"/macos/App/CandidatePanel.swift \
    "${ROOT_DIR}"/macos/App/EngineBridge.swift \
    "${ROOT_DIR}"/macos/App/Keysym.swift \
    "${ROOT_DIR}"/macos/App/LlavonInputController.swift \
    "${ROOT_DIR}"/macos/App/SettingsWindow.swift \
    "${ROOT_DIR}"/macos/App/TextClient.swift \
    "${ROOT_DIR}"/macos/Tests/InputHarness.swift \
    "${ENGINE_BUILD_DIR}/libllavon_ime_engine.a" \
    -lc++ \
    -framework Cocoa \
    -framework InputMethodKit \
    -framework Carbon \
    -o "${BUILD_DIR}/input-harness"

LLAVON_IME_TABLE_PATH="${LLAVON_IME_TABLE_PATH:-${ROOT_DIR}/ime-unix-service/ime-core/table/bopomofo_char.json}" \
    "${BUILD_DIR}/input-harness"
