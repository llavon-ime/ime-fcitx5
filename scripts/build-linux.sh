#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_FILE="llavon-ime-llama-250m-Q4_K_M.gguf"
MODEL_URL="${IME_FCITX5_MODEL_URL:-https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${MODEL_FILE}}"
MODEL_DIR="${IME_FCITX5_MODEL_DIR:-${ROOT_DIR}/models}"
MODEL_PATH="${MODEL_DIR}/${MODEL_FILE}"
MODEL_INSTALL_PATH="/usr/share/llavon-ime/models/${MODEL_FILE}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script only supports Linux." >&2
    exit 2
fi

case "$(uname -m)" in
    x86_64 | amd64) ;;
    *)
        echo "The linux presets currently only support x86_64." >&2
        exit 2
        ;;
esac

for command in git cmake curl install pkg-config sed; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        exit 2
    fi
done

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

if [[ ! -s "${MODEL_PATH}" ]]; then
    echo "Downloading model to ${MODEL_PATH}..."
    mkdir -p "${MODEL_DIR}"
    MODEL_PART_PATH="${MODEL_PATH}.part"
    trap 'rm -f "${MODEL_PART_PATH}"' EXIT
    curl --fail --location --retry 3 --output "${MODEL_PART_PATH}" "${MODEL_URL}"
    test -s "${MODEL_PART_PATH}"
    mv "${MODEL_PART_PATH}" "${MODEL_PATH}"
    trap - EXIT
else
    echo "Using existing model: ${MODEL_PATH}"
fi

echo "Building and testing ime-unix-service..."
(
    cd "${ROOT_DIR}/ime-unix-service"
    cmake --preset linux -DIME_UNIX_SERVICE_BUILD_TESTS=ON
    cmake --build --preset linux --parallel
    ctest --test-dir build/linux --output-on-failure
)

echo "Building and testing fcitx5 addon..."
(
    cd "${ROOT_DIR}/fcitx5"
    cmake --preset linux \
        -DIME_FCITX5_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}" \
        -DIME_FCITX5_DISPLAY_VERSION="${DISPLAY_VERSION}"
    cmake --build --preset linux --parallel
    ctest --preset linux
)

echo "Installing ime-unix-service, fcitx5 addon, and model..."
"${SUDO[@]}" cmake --install "${ROOT_DIR}/ime-unix-service/build/linux"
"${SUDO[@]}" cmake --install "${ROOT_DIR}/build/fcitx5"
"${SUDO[@]}" install -Dm644 "${MODEL_PATH}" "${MODEL_INSTALL_PATH}"

echo "Linux build, tests, and installation completed successfully."
