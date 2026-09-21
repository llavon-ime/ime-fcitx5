#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1

# Builds the native InputMethodKit frontend: engine static library, Swift app
# bundle, ad-hoc signature, optionally installing into the user's input
# methods. See macos/README.md for the manual test procedure.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_NAME="LlavonIME"
BUNDLE_ID="${LLAVON_IME_BUNDLE_ID:-com.llavon.inputmethod.LlavonIME}"
VERSION="${LLAVON_IME_VERSION:-}"
BUILD_DIR="${LLAVON_IME_NATIVE_BUILD_DIR:-${ROOT_DIR}/build/native-macos}"
ENGINE_BUILD_DIR="${BUILD_DIR}/engine"
DIST_DIR="${LLAVON_IME_NATIVE_DIST_DIR:-${ROOT_DIR}/dist/macos}"
APP_DIR="${DIST_DIR}/${APP_NAME}.app"
INSTALL=0

for argument in "$@"; do
    case "${argument}" in
        --install) INSTALL=1 ;;
        *)
            echo "Unknown argument: ${argument}" >&2
            echo "Usage: $0 [--install]" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script only supports macOS." >&2
    exit 2
fi
for command in cmake swiftc codesign; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        if [[ "${command}" == "cmake" ]]; then
            echo "Install it with: brew install cmake" >&2
        fi
        exit 2
    fi
done
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "Required command not found: pkg-config" >&2
    echo "Install it with: brew install pkg-config" >&2
    exit 2
fi

if [[ -z "${VERSION}" ]]; then
    VERSION="$(git -C "${ROOT_DIR}" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)"
fi
VERSION="${VERSION:-0.1.0}"

if [[ ! -f "${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Initializing the vcpkg submodule..."
    git -C "${ROOT_DIR}" submodule update --init vcpkg
fi
if [[ ! -x "${ROOT_DIR}/vcpkg/vcpkg" ]]; then
    echo "Bootstrapping vcpkg..."
    "${ROOT_DIR}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

echo "Building the engine..."
cmake -S "${ROOT_DIR}/engine" -B "${ENGINE_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DLLAVON_IME_ENGINE_BUILD_TESTS=OFF
cmake --build "${ENGINE_BUILD_DIR}" --target llavon_ime_engine --parallel

echo "Compiling the input method..."
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources"

swiftc -O -parse-as-library \
    -module-name LlavonIMEApp \
    -I "${ROOT_DIR}/engine/include" \
    "${ROOT_DIR}"/macos/Core/*.swift \
    "${ROOT_DIR}"/macos/App/*.swift \
    "${ENGINE_BUILD_DIR}/libllavon_ime_engine.a" \
    -lc++ \
    -framework Cocoa \
    -framework InputMethodKit \
    -framework Carbon \
    -o "${APP_DIR}/Contents/MacOS/${APP_NAME}"

sed -e "s/@APP_NAME@/${APP_NAME}/g" \
    -e "s/@BUNDLE_ID@/${BUNDLE_ID}/g" \
    -e "s/@VERSION@/${VERSION}/g" \
    "${ROOT_DIR}/macos/App/Info.plist.in" > "${APP_DIR}/Contents/Info.plist"
printf 'APPL????' > "${APP_DIR}/Contents/PkgInfo"
for icon in "${ROOT_DIR}"/macos/App/MenuIcon*.png; do
    [[ -f "${icon}" ]] || continue
    cp "${icon}" "${APP_DIR}/Contents/Resources/$(basename "${icon}")"
done

if [[ -f "${ROOT_DIR}/macos/App/AppIcon.png" ]] &&
   command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    iconset="$(mktemp -d)/AppIcon.iconset"
    mkdir -p "${iconset}"
    for size in 16 32 128 256 512; do
        sips -z "${size}" "${size}" "${ROOT_DIR}/macos/App/AppIcon.png" \
            --out "${iconset}/icon_${size}x${size}.png" >/dev/null
        sips -z "$((size * 2))" "$((size * 2))" "${ROOT_DIR}/macos/App/AppIcon.png" \
            --out "${iconset}/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns -o "${APP_DIR}/Contents/Resources/AppIcon.icns" "${iconset}"
    rm -rf "$(dirname "${iconset}")"
fi
for lproj in Base zh-Hant; do
    mkdir -p "${APP_DIR}/Contents/Resources/${lproj}.lproj"
    sed -e "s/@BUNDLE_ID@/${BUNDLE_ID}/g" \
        "${ROOT_DIR}/macos/App/InfoPlist.strings.in" \
        > "${APP_DIR}/Contents/Resources/${lproj}.lproj/InfoPlist.strings"
done

echo "Signing (ad-hoc)..."
codesign --force --deep --sign - "${APP_DIR}"

if [[ "${INSTALL}" == "1" ]]; then
    destination="${HOME}/Library/Input Methods/${APP_NAME}.app"
    echo "Installing to ${destination}..."
    pkill -x "${APP_NAME}" 2>/dev/null || true
    rm -rf "${destination}"
    cp -R "${APP_DIR}" "${destination}"
    killall TextInputMenuAgent 2>/dev/null || true
    # TextInputSwitcher and CursorUIViewService ignore SIGTERM and cache input
    # source icons in memory, so a stale instance keeps showing an icon-less
    # switcher HUD / caret indicator.
    killall -9 TextInputSwitcher 2>/dev/null || true
    killall -9 CursorUIViewService 2>/dev/null || true
    cat <<'EOF'
Installed. Select 「拉風輸入法」 under System Settings > Keyboard > Input Sources.
The input method restarts on next use, so the settings menu entries appear
without logging out. If it does not appear, log out and back in once.
EOF
fi

echo "Built ${APP_DIR}"
