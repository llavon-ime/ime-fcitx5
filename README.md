# Llavon IME for Fcitx5

Linux and macOS fcitx5 frontend for Llavon IME. Inference runs in the `ime-service`
submodule as a standalone `llavon-ime-service` process with session-based Unix
socket IPC.

## Linux Build

Install CMake, Ninja, pkg-config, fcitx5 development files, and initialize
submodules:

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-fcitx5.git
cd ime-fcitx5
./vcpkg/bootstrap-vcpkg.sh

cmake -S ime-service -B build/ime-service -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DIMESVC_BUILD_TESTS=ON
cmake --build build/ime-service
ctest --test-dir build/ime-service --output-on-failure
sudo cmake --install build/ime-service

cmake -S fcitx5 -B build/fcitx5 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build/fcitx5
sudo cmake --install build/fcitx5
```

Enable `llavon-ime` in the fcitx5 configuration tool, then restart fcitx5 with
`fcitx5 -r`. The addon starts `llavon-ime-service` on demand.

## macOS Build

Install fcitx5-macos and clone its source checkout for headers, then build the
service and addon:

```bash
cmake -S ime-service -B build/ime-service-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_INSTALL_PREFIX="$HOME/Library/fcitx5" \
  -DVCPKG_MANIFEST_FEATURES=llama-metal
cmake --build build/ime-service-macos
cmake --install build/ime-service-macos

cmake -S fcitx5 -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DFCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos \
  -DCMAKE_INSTALL_PREFIX="$HOME/Library/fcitx5"
cmake --build build/macos
cmake --install build/macos
```

## Model

Release packages include the Q4 GGUF model. Development builds require a local
model configured through the fcitx5 settings page or `IME_FCITX5_MODEL_PATH`.

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF
