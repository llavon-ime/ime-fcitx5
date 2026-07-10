# Llavon IME for Fcitx5

Linux and macOS fcitx5 frontend and local llama.cpp inference service for Llavon IME.

The repository includes the fcitx5 addon, the shared bopomofo/token tables, AUR package metadata, and macOS release packaging.

## Linux Build

Install CMake, Ninja, pkg-config, fcitx5 development files, and initialize the vcpkg submodule:

```bash
git clone --recurse-submodules git@github.com:llavon-ime/ime-fcitx5.git
cd ime-fcitx5
./vcpkg/bootstrap-vcpkg.sh
cmake -S fcitx5 -B build/fcitx5 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build/fcitx5
sudo cmake --install build/fcitx5
```

Enable `llavon-ime` in the fcitx5 configuration tool, then restart fcitx5 with `fcitx5 -r`.

## macOS Build

Install fcitx5-macos and clone its source checkout for headers, then configure with its source path:

```bash
cmake -S fcitx5 -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DFCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos \
  -DCMAKE_INSTALL_PREFIX="$HOME/Library/fcitx5"
cmake --build build/macos
cmake --install build/macos
```

Use `-DVCPKG_MANIFEST_FEATURES=llama-metal` for Metal GPU offload.

## Model

Release packages include the Q4 GGUF model. Development builds require a local model configured through the fcitx5 settings page or `IME_FCITX5_MODEL_PATH`.

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF
