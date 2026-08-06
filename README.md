# Llavon IME for Fcitx5

Linux and macOS fcitx5 frontend for Llavon IME. Inference runs in the
`ime-unix-service` submodule as a standalone `llavon-ime-unix-service` process
with session-based Unix socket IPC. The service uses its nested `ime-core`
submodule for model loading, tokenization, and llama.cpp inference.

## Linux Build

Install CMake, pkg-config, fcitx5 development files, and initialize
submodules:

```bash
git clone --recurse-submodules https://github.com/llavon-ime/ime-fcitx5.git
cd ime-fcitx5
./vcpkg/bootstrap-vcpkg.sh

cd ime-unix-service
cmake --preset linux -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cd ..
sudo cmake --install ime-unix-service/build/linux

cd fcitx5
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
cd ..
sudo cmake --install build/fcitx5
```

Presets are defined per project:

- `ime-unix-service/CMakePresets.json` — service builds install to `/usr`
  (Linux) / `$HOME/Library/fcitx5` (macOS), enable GPU offload via the
  `llama-vulkan` (Linux) / `llama-metal` (macOS) manifest features, and use the
  `x64-linux-llavon` / `arm64-osx-llavon` vcpkg triplets, which enable
  `GGML_VULKAN=ON` and `GGML_NATIVE=ON` for the bundled llama.cpp/ggml.
- `fcitx5/CMakePresets.json` — addon builds install to `/usr` like the AUR
  package.

Both presets default to the repository vcpkg toolchain and do not pin a
generator: CMake picks a default (e.g. Ninja, Unix Makefiles) and you can
still pass `-G Ninja` or `-G "Unix Makefiles"` explicitly.

Enable `llavon-ime` in the fcitx5 configuration tool, then restart fcitx5 with
`fcitx5 -r`. The addon starts `llavon-ime-unix-service` on demand.

## macOS Build

Install fcitx5-macos and clone its source checkout for headers, then build the
service and addon. Set `FCITX5_MACOS_SOURCE_DIR` to the fcitx5-macos checkout;
the find module reads this environment variable:

```bash
export FCITX5_MACOS_SOURCE_DIR=/path/to/fcitx5-macos

cd ime-unix-service
cmake --preset macos
cmake --build --preset macos
cmake --install build/macos
cd ..

cd fcitx5
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
cd ..
cmake --install build/macos
```

On macOS the presets install into `$HOME/Library/fcitx5`, the directory the
`postinstall` script of the release package copies the payload into. The
`arm64-osx-llavon` vcpkg triplet enables `GGML_NATIVE=ON` and the preset
selects the Metal backend via the `llama-metal` manifest feature.

## Model

Release packages include the Q4 GGUF model. Development builds require a local
model configured through the fcitx5 settings page or `IME_FCITX5_MODEL_PATH`.
The bundled model is licensed under CC BY-NC 4.0 and is limited to
non-commercial use. Release packages include its attribution notice and the
software dependency licenses.

https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF
