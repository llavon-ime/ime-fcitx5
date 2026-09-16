#!/usr/bin/env bash
set -euo pipefail

# Builds and tests the Unix service and the Fcitx5 addon from this checkout
# (including local changes), then installs them into ~/Library/fcitx5. The
# model is installed at the package default location and reused when present.
#
# The fcitx5 headers come from fcitx5-macos, which is pulled into a temp
# folder when FCITX5_MACOS_SOURCE_DIR does not point at an existing checkout.
# The installed Fcitx5.app dylibs are required as well.
#
# Environment overrides:
#   FCITX5_MACOS_SOURCE_DIR   use this fcitx5-macos checkout instead of pulling
#                             one into $TMPDIR
#   IME_FCITX5_FCITX5_MACOS_REPO_URL  fcitx5-macos repository to pull
#   IME_FCITX5_MODEL_URL      model mirror
#   IME_FCITX5_VERSION        display version override
#   LLAVON_IME_DEBUG          any non-empty value compiles in debug logging

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_FILE="llavon-ime-llama-250m-Q4_K_M.gguf"
MODEL_URL="${IME_FCITX5_MODEL_URL:-https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${MODEL_FILE}}"
MODEL_INSTALL_PATH="/Library/Application Support/llavon-ime/models/${MODEL_FILE}"
FCITX5_APP_CONTENTS="/Library/Input Methods/Fcitx5.app/Contents"
FCITX5_MACOS_REPO_URL="${IME_FCITX5_FCITX5_MACOS_REPO_URL:-https://github.com/fcitx/fcitx5-macos.git}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi

case "$(uname -m)" in
    arm64) ;;
    *)
        echo "The macos presets currently only support Apple Silicon (arm64)." >&2
        exit 2
        ;;
esac

for command in git cmake curl install sed pkg-config unzip; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
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

if [[ -z "${FCITX5_MACOS_SOURCE_DIR:-}" ]]; then
    FCITX5_MACOS_SOURCE_DIR="${TMPDIR:-/tmp}/llavon-ime-fcitx5-macos"
    if [[ -d "${FCITX5_MACOS_SOURCE_DIR}/.git" ]]; then
        echo "Updating fcitx5-macos in ${FCITX5_MACOS_SOURCE_DIR}..."
        git -C "${FCITX5_MACOS_SOURCE_DIR}" pull --ff-only
    else
        echo "Pulling fcitx5-macos into ${FCITX5_MACOS_SOURCE_DIR}..."
        git clone --depth 1 "${FCITX5_MACOS_REPO_URL}" "${FCITX5_MACOS_SOURCE_DIR}"
    fi
    git -C "${FCITX5_MACOS_SOURCE_DIR}" submodule update --init --depth 1 fcitx5
fi
if [[ ! -f "${FCITX5_MACOS_SOURCE_DIR}/fcitx5/src/lib/fcitx/inputmethodengine.h" ]]; then
    echo "fcitx5 headers were not found under: ${FCITX5_MACOS_SOURCE_DIR}" >&2
    echo "If this is a fcitx5-macos checkout, run:" >&2
    echo "  git -C \"${FCITX5_MACOS_SOURCE_DIR}\" submodule update --init fcitx5" >&2
    exit 2
fi
if [[ ! -f "${FCITX5_APP_CONTENTS}/lib/libFcitx5Core.dylib" ]]; then
    echo "Fcitx5.app was not found at ${FCITX5_APP_CONTENTS}." >&2
    echo "Install fcitx5-macos first; it provides the InputMethodKit host and the libFcitx5 dylibs." >&2
    exit 2
fi

DISPLAY_VERSION="${IME_FCITX5_VERSION:-}"
if [[ -z "${DISPLAY_VERSION}" ]]; then
    describe="$(git -C "${ROOT_DIR}" describe --long --tags --abbrev=7 2>/dev/null || true)"
    if [[ -n "${describe}" ]]; then
        DISPLAY_VERSION="$(printf '%s\n' "${describe}" | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g')"
    else
        base_version="$(sed -n 's/^project(llavon-ime VERSION \([^ ]*\).*/\1/p' "${ROOT_DIR}/fcitx5/CMakeLists.txt")"
        base_version="${base_version:-0.1.0}"
        DISPLAY_VERSION="${base_version}.r$(git -C "${ROOT_DIR}" rev-list --count HEAD).g$(git -C "${ROOT_DIR}" rev-parse --short=7 HEAD)"
    fi
fi
echo "Llavon IME display version: ${DISPLAY_VERSION}"

SUDO=()
if ((EUID != 0)); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Required command not found: sudo" >&2
        exit 2
    fi
    SUDO=(sudo)
fi

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ||
      ! -f "${ROOT_DIR}/ime-unix-service/CMakeLists.txt" ||
      ! -f "${ROOT_DIR}/ime-unix-service/ime-core/CMakeLists.txt" ]]; then
    echo "Initializing git submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# The release package installs the model globally; detect it directly there
# and only download when it is missing.
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

echo "Building and testing fcitx5 addon..."
(
    cd "${ROOT_DIR}/fcitx5"
    cmake --preset macos \
        -DFCITX5_MACOS_SOURCE_DIR="${FCITX5_MACOS_SOURCE_DIR}" \
        -DIME_FCITX5_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}" \
        -DIME_FCITX5_DISPLAY_VERSION="${DISPLAY_VERSION}" \
        ${LLAVON_DEBUG_FLAG}
    cmake --build --preset macos --parallel
    ctest --preset macos
)

echo "Installing ime-unix-service and fcitx5 addon into ${HOME}/Library/fcitx5..."
cmake --install "${ROOT_DIR}/ime-unix-service/build/macos"
cmake --install "${ROOT_DIR}/build/macos"

cat <<EOF

macOS build, tests, and installation completed successfully.

Next steps:
  * Restart the input method so the new addon is loaded:
      pkill -x Fcitx5; open -gj -b org.fcitx.inputmethod.Fcitx5
  * Use Fcitx5.app 0.3.4 or newer so InputMethodKit surrounding text is sent
    to the addon. No Accessibility permission is required.
EOF
