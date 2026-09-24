#!/usr/bin/env bash
set -euo pipefail

# Builds and tests the AI prediction service (ime-unix-service) from this
# checkout and installs it where the native macOS input method looks for it.
# The model is installed at the package default location and reused when
# present.
#
# The input method frontend itself is built by macos/scripts/build-native-app.sh,
# which calls this script with LLAVON_IME_SERVICE_INSTALL_PREFIX when it
# installs the app. It works without this service (table candidates are the
# fallback, predictions are additive), so only run this when you want the
# llama service.
#
# Currently Apple Silicon only: the macos preset uses the arm64-osx-llavon
# triplet with the Metal backend.
#
# Environment overrides:
#   LLAVON_IME_MODEL_URL                model mirror
#   LLAVON_IME_DEBUG                    any non-empty value compiles in debug logging
#   LLAVON_IME_SERVICE_INSTALL_PREFIX   install prefix (default ~/Library/fcitx5;
#                                       the package payload path installs with sudo)
#   LLAVON_IME_SERVICE_SKIP_NEXT_STEPS  set to 1 to drop the closing hints

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_FILE="llavon-ime-llama-250m-Q4_K_M.gguf"
MODEL_URL="${LLAVON_IME_MODEL_URL:-https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${MODEL_FILE}}"
MODEL_INSTALL_PATH="/Library/Application Support/llavon-ime/models/${MODEL_FILE}"
INSTALL_PREFIX="${LLAVON_IME_SERVICE_INSTALL_PREFIX:-${HOME}/Library/fcitx5}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

case "$(uname -m)" in
    arm64) ;;
    *)
        echo "The macos preset currently only supports Apple Silicon (arm64)." >&2
        exit 2
        ;;
esac

for command in git cmake curl install sed pkg-config unzip; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "pkg-config" ]]; then
            echo "Install it with: brew install pkg-config" >&2
        fi
        exit 2
    fi
done

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode command line tools not found; run: xcode-select --install" >&2
    exit 2
fi
if ! command -v clang++ >/dev/null 2>&1; then
    echo "Required compiler not found: clang++" >&2
    exit 2
fi
if ! command -v ninja >/dev/null 2>&1; then
    echo "Note: ninja not found; CMake will fall back to Unix Makefiles." >&2
fi

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/ime-core/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/lora-trainer/.git" ]]; then
    echo "Initializing git submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi
if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "vcpkg was not found; run: git -C \"${ROOT_DIR}\" submodule update --init vcpkg" >&2
    exit 2
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# The release package installs the model globally; detect it directly there
# and only download when it is missing.
SUDO=()
if ((EUID != 0)); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Required command not found: sudo" >&2
        exit 2
    fi
    SUDO=(sudo)
fi

if [[ -s "${MODEL_INSTALL_PATH}" ]]; then
    echo "Using installed model: ${MODEL_INSTALL_PATH}"
else
    echo "Downloading model to ${MODEL_INSTALL_PATH}..."
    "${SUDO[@]}" mkdir -p "/Library/Application Support/llavon-ime/models"
    MODEL_PART_PATH="${TMPDIR:-/tmp}/${MODEL_FILE}.part"
    trap 'rm -f "${MODEL_PART_PATH}"' EXIT
    curl --fail --location --retry 3 --output "${MODEL_PART_PATH}" "${MODEL_URL}"
    test -s "${MODEL_PART_PATH}"
    "${SUDO[@]}" install -m 0644 "${MODEL_PART_PATH}" "${MODEL_INSTALL_PATH}"
    rm -f "${MODEL_PART_PATH}"
    trap - EXIT
fi

LLAVON_DEBUG_FLAG=""
if [[ -n "${LLAVON_IME_DEBUG:-}" ]]; then
    LLAVON_DEBUG_FLAG="-DLLAVON_IME_DEBUG=ON"
fi

echo "Building and testing ime-unix-service (Metal; the first build can take a while)..."
(
    cd "${ROOT_DIR}/ime-unix-service"
    cmake --preset macos -DIME_UNIX_SERVICE_BUILD_TESTS=ON ${LLAVON_DEBUG_FLAG}
    cmake --build --preset macos --parallel
    ctest --test-dir build/macos --output-on-failure
)

echo "Installing ime-unix-service into ${INSTALL_PREFIX}..."
if [[ "${INSTALL_PREFIX}" == "${HOME}"/* ]]; then
    cmake --install "${ROOT_DIR}/ime-unix-service/build/macos" --prefix "${INSTALL_PREFIX}"
else
    # The package payload lives outside the home directory.
    "${SUDO[@]}" cmake --install "${ROOT_DIR}/ime-unix-service/build/macos" --prefix "${INSTALL_PREFIX}"
fi

if [[ "${LLAVON_IME_SERVICE_SKIP_NEXT_STEPS:-}" != "1" ]]; then
    cat <<EOF

Service build, tests, and installation completed successfully.

Next steps:
  * Build and install the input method frontend:
      macos/scripts/build-native-app.sh --install
  * The app finds the service at ${INSTALL_PREFIX}/bin/llavon-ime-unix-service
    and the tables at ${INSTALL_PREFIX}/share/llavon-ime/tables.
EOF
fi
