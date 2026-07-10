#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${IME_FCITX5_VERSION:-0.1.0}"
ARCH="$(uname -m)"
PAYLOAD_PREFIX="${IME_FCITX5_MACOS_PAYLOAD_PREFIX:-/Library/Application Support/llavon-ime/payload}"
BUILD_DIR="${IME_FCITX5_BUILD_DIR:-${ROOT_DIR}/build/package-llavon-ime-macos-${ARCH}}"
DIST_DIR="${IME_FCITX5_DIST_DIR:-${ROOT_DIR}/dist/macos}"
PKGROOT="${DIST_DIR}/pkgroot"
PKG_IDENTIFIER="${IME_FCITX5_PKG_IDENTIFIER:-llavon-ime}"
VCPKG_FEATURES="${IME_FCITX5_VCPKG_FEATURES:-llama-metal}"
FCITX5_MACOS_SOURCE_DIR="${FCITX5_MACOS_SOURCE_DIR:-${1:-}}"
MODEL_PATH="${IME_FCITX5_PACKAGE_MODEL_PATH:-}"
MODEL_INSTALL_DIR="/Library/Application Support/llavon-ime/models"
MODEL_INSTALL_PATH=""

if [[ -z "${FCITX5_MACOS_SOURCE_DIR}" && -d "${ROOT_DIR}/../fcitx5-macos" ]]; then
    FCITX5_MACOS_SOURCE_DIR="${ROOT_DIR}/../fcitx5-macos"
fi

if [[ -z "${FCITX5_MACOS_SOURCE_DIR}" ]]; then
    cat >&2 <<'EOF'
FCITX5_MACOS_SOURCE_DIR is required.

Usage:
  FCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos ./scripts/package-macos.sh
  ./scripts/package-macos.sh /path/to/fcitx5-macos
EOF
    exit 2
fi

if [[ ! -f "${FCITX5_MACOS_SOURCE_DIR}/fcitx5/src/lib/fcitx/inputmethodengine.h" && ! -f "${FCITX5_MACOS_SOURCE_DIR}/src/lib/fcitx/inputmethodengine.h" ]]; then
    cat >&2 <<EOF
fcitx5 headers were not found under: ${FCITX5_MACOS_SOURCE_DIR}
If this is a fcitx5-macos checkout, run:
  git -C "${FCITX5_MACOS_SOURCE_DIR}" submodule update --init fcitx5
EOF
    exit 2
fi

if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh"
fi

if [[ -z "${MODEL_PATH}" ]]; then
    shopt -s nullglob
    model_candidates=("${ROOT_DIR}"/models/*.gguf)
    shopt -u nullglob
    if [[ ${#model_candidates[@]} -eq 1 ]]; then
        MODEL_PATH="${model_candidates[0]}"
    elif [[ ${#model_candidates[@]} -eq 0 ]]; then
        cat >&2 <<'EOF'
No .gguf model was found under models/.

Set IME_FCITX5_PACKAGE_MODEL_PATH=/path/to/model.gguf to build the installer with a bundled model.
EOF
        exit 2
    else
        cat >&2 <<'EOF'
Multiple .gguf models were found under models/.

Set IME_FCITX5_PACKAGE_MODEL_PATH=/path/to/model.gguf to choose the model bundled in the installer.
EOF
        exit 2
    fi
fi

if [[ ! -f "${MODEL_PATH}" || "${MODEL_PATH}" != *.gguf ]]; then
    echo "IME_FCITX5_PACKAGE_MODEL_PATH must point to a .gguf file: ${MODEL_PATH}" >&2
    exit 2
fi

MODEL_INSTALL_PATH="${MODEL_INSTALL_DIR}/$(basename "${MODEL_PATH}")"

rm -rf "${PKGROOT}"
mkdir -p "${PKGROOT}" "${DIST_DIR}"

cmake_args=(
    -S "${ROOT_DIR}/fcitx5"
    -B "${BUILD_DIR}"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    -DIME_FCITX5_BUILD_TESTS=ON
    -DFCITX5_MACOS_SOURCE_DIR="${FCITX5_MACOS_SOURCE_DIR}"
    -DCMAKE_INSTALL_PREFIX="${PAYLOAD_PREFIX}"
    -DFCITX_INSTALL_ADDONDIR=lib/fcitx5
    -DFCITX_INSTALL_PKGDATADIR=share/fcitx5
    -DIME_FCITX5_FCITX_PLUGIN_DIR=plugin
    -DIME_FCITX5_INSTALLED_MODEL_PATH="${MODEL_INSTALL_PATH}"
)

if [[ -n "${VCPKG_FEATURES}" ]]; then
    cmake_args+=(-DVCPKG_MANIFEST_FEATURES="${VCPKG_FEATURES}")
fi

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
DESTDIR="${PKGROOT}" cmake --install "${BUILD_DIR}"

payload_root="${PKGROOT}${PAYLOAD_PREFIX}"
model_root="${PKGROOT}${MODEL_INSTALL_DIR}"
mkdir -p "${model_root}"
install -m 0644 "${MODEL_PATH}" "${model_root}/$(basename "${MODEL_PATH}")"

if command -v xattr >/dev/null 2>&1; then
    xattr -cr "${PKGROOT}" || true
fi
find "${PKGROOT}" -name '._*' -delete

required_files=(
    "${payload_root}/bin/llavon-ime-service"
    "${payload_root}/lib/fcitx5/llavon-ime-addon.so"
    "${payload_root}/share/fcitx5/addon/llavon-ime.conf"
    "${payload_root}/share/fcitx5/inputmethod/llavon-ime.conf"
    "${payload_root}/share/llavon-ime/tables/bopomofo_char.json"
    "${payload_root}/share/llavon-ime/tables/tokens/bpmf.json"
    "${payload_root}/share/llavon-ime/tables/tokens/chars.json"
    "${payload_root}/share/llavon-ime/tables/tokens/latin.json"
    "${payload_root}/share/llavon-ime/tables/tokens/special_tokens.json"
    "${payload_root}/plugin/llavon-ime.json"
    "${PKGROOT}${MODEL_INSTALL_PATH}"
)

for file in "${required_files[@]}"; do
    if [[ ! -f "${file}" ]]; then
        echo "Missing package payload file: ${file}" >&2
        exit 1
    fi
done

if [[ -e "${PKGROOT}/Users" ]]; then
    echo "Package root unexpectedly contains /Users; install paths are not relocatable." >&2
    exit 1
fi

if [[ -n "${DEVELOPER_ID_APPLICATION:-}" ]]; then
    codesign --force --timestamp --options runtime --sign "${DEVELOPER_ID_APPLICATION}" \
        "${payload_root}/bin/llavon-ime-service" \
        "${payload_root}/lib/fcitx5/llavon-ime-addon.so"
fi

unsigned_pkg="${DIST_DIR}/llavon-ime-${VERSION}-${ARCH}.pkg"
pkgbuild \
    --root "${PKGROOT}" \
    --scripts "${ROOT_DIR}/packaging/macos/scripts" \
    --identifier "${PKG_IDENTIFIER}" \
    --version "${VERSION}" \
    --install-location / \
    "${unsigned_pkg}"

if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    signed_pkg="${DIST_DIR}/llavon-ime-${VERSION}-${ARCH}-signed.pkg"
    productsign --sign "${DEVELOPER_ID_INSTALLER}" "${unsigned_pkg}" "${signed_pkg}"
    echo "Built signed package: ${signed_pkg}"
else
    echo "Built package: ${unsigned_pkg}"
fi
echo "Bundled model: ${MODEL_INSTALL_PATH}"
