# Maintainer: llavon-ime contributors

_pkgname=llavon-ime-fcitx5
_srcname=ime-fcitx5
_model_file=llavon-ime-llama-250m-Q4_K_M.gguf
pkgname=${_pkgname}-git
pkgver=0.1.0.r3.g78fdc91
pkgrel=1
pkgdesc='Fcitx5 frontend and local inference service for Llavon IME'
arch=('x86_64' 'aarch64')
url='https://github.com/llavon-ime/ime-fcitx5'
license=('BSD-2-Clause')
depends=('asio' 'fcitx5' 'llama.cpp' 'nlohmann-json')
makedepends=('cmake' 'git' 'ninja' 'pkgconf')
optdepends=('fcitx5-configtool: graphical configuration for fcitx5')
provides=("${_pkgname}")
conflicts=("${_pkgname}")
source=(
    "${_srcname}::git+https://github.com/llavon-ime/ime-fcitx5.git#branch=main"
    "${_model_file}::https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/${_model_file}"
)
sha256sums=('SKIP' 'SKIP')

pkgver() {
    cd "${_srcname}"

    local describe base_version
    describe="$(git describe --long --tags --abbrev=7 2>/dev/null || true)"
    if [[ -n "${describe}" ]]; then
        printf '%s\n' "${describe}" | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
        return
    fi

    base_version="$(sed -n 's/^project(llavon-ime VERSION \([^ ]*\).*/\1/p' fcitx5/CMakeLists.txt)"
    base_version="${base_version:-0.1.0}"
    printf '%s.r%s.g%s\n' "${base_version}" "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

build() {
    cmake -S "${_srcname}/fcitx5" -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=None \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DIME_FCITX5_INSTALLED_MODEL_PATH="/usr/share/llavon-ime/models/${_model_file}" \
        -DIME_FCITX5_BUILD_TESTS=OFF
    cmake --build build
}

package() {
    DESTDIR="${pkgdir}" cmake --install build
    install -Dm644 "${srcdir}/${_model_file}" \
        "${pkgdir}/usr/share/llavon-ime/models/${_model_file}"
    install -Dm644 "${_srcname}/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
