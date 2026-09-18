#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${IME_FCITX5_VERSION:-0.2.1}"
ARCH="$(uname -m)"
FCITX5_MACOS_VERSION="${FCITX5_MACOS_VERSION:-0.3.7}"
INSTALLER_REPO="${IME_FCITX5_MACOS_INSTALLER_REPO:-https://github.com/llavon-ime/fcitx5-macos-installer.git}"
INSTALLER_REF="${IME_FCITX5_MACOS_INSTALLER_REF:-llavon-ime}"
INSTALLER_SRC_DIR="${IME_FCITX5_MACOS_INSTALLER_SRC_DIR:-${ROOT_DIR}/build/macos-installer-src}"
DIST_DIR="${IME_FCITX5_DIST_DIR:-${ROOT_DIR}/dist/macos}"
PLUGIN_DIST_DIR="${DIST_DIR}/plugin"
RELEASE_REPOSITORY="${IME_FCITX5_RELEASE_REPOSITORY:-llavon-ime/ime-fcitx5}"
OUTPUT_ZIP_NAME="${IME_FCITX5_INSTALLER_ZIP_NAME:-llavon-ime-installer-${VERSION}-${ARCH}.zip}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
MODEL_PATH="${IME_FCITX5_PACKAGE_MODEL_PATH:-}"

if [[ "${ARCH}" != "arm64" ]]; then
    echo "The macOS installer is only built for Apple Silicon (arm64); current architecture: ${ARCH}." >&2
    exit 2
fi

for required in \
    "${PLUGIN_DIST_DIR}/llavon-ime-${ARCH}.tar.bz2" \
    "${PLUGIN_DIST_DIR}/llavon-ime-any.tar.bz2"; do
    if [[ ! -f "${required}" ]]; then
        echo "Missing plugin tarball: ${required}" >&2
        echo "Run ./scripts/package-macos.sh first to build the payload and plugin tarballs." >&2
        exit 2
    fi
done

if [[ -z "${MODEL_PATH}" ]]; then
    shopt -s nullglob
    model_candidates=("${ROOT_DIR}"/models/*.gguf)
    shopt -u nullglob
    if [[ ${#model_candidates[@]} -eq 1 ]]; then
        MODEL_PATH="${model_candidates[0]}"
    else
        echo "Set IME_FCITX5_PACKAGE_MODEL_PATH=/path/to/model.gguf to choose the bundled model." >&2
        exit 2
    fi
fi

if [[ ! -f "${MODEL_PATH}" || "${MODEL_PATH}" != *.gguf ]]; then
    echo "IME_FCITX5_PACKAGE_MODEL_PATH must point to a .gguf file: ${MODEL_PATH}" >&2
    exit 2
fi

if [[ ! -d "${INSTALLER_SRC_DIR}/.git" ]]; then
    mkdir -p "$(dirname "${INSTALLER_SRC_DIR}")"
    git clone "${INSTALLER_REPO}" "${INSTALLER_SRC_DIR}"
fi
git -C "${INSTALLER_SRC_DIR}" fetch --quiet origin "${INSTALLER_REF}"
git -C "${INSTALLER_SRC_DIR}" checkout --quiet --detach FETCH_HEAD
git -C "${INSTALLER_SRC_DIR}" submodule update --init --recursive --quiet

(
    cd "${INSTALLER_SRC_DIR}"
    INSTALLER_ARCHES="${ARCH}" \
    PLUGIN_LOCAL_DIR="${PLUGIN_DIST_DIR}" \
    PLUGIN_BASE_URL="https://github.com/${RELEASE_REPOSITORY}/releases/download/v${VERSION}/" \
    MODEL_LOCAL_PATH="${MODEL_PATH}" \
    OUTPUT_ZIP_NAME="${OUTPUT_ZIP_NAME}" \
    "${PYTHON_BIN}" package.py "${FCITX5_MACOS_VERSION}" "Llavon IME" llavon-ime llavon-ime true
)

installer_zip="${INSTALLER_SRC_DIR}/build/universal/src/${OUTPUT_ZIP_NAME}"
if [[ ! -f "${installer_zip}" ]]; then
    echo "Installer archive was not produced: ${installer_zip}" >&2
    exit 1
fi
install -m 0644 "${installer_zip}" "${DIST_DIR}/${OUTPUT_ZIP_NAME}"
echo "Built installer: ${DIST_DIR}/${OUTPUT_ZIP_NAME}"
