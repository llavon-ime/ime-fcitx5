# IME Service

Standalone build for the IME service executable. Linux and macOS use the per-user Unix socket server; Windows uses the
named-pipe server. The optional native LoRA trainer is a separate install component and process.

## Build

This repository owns only `vcpkg.json`. It does not vendor vcpkg and should not point at a local absolute vcpkg path.
Pass the vcpkg toolchain from the build environment, CI, or parent build orchestration:

```powershell
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset windows
cmake --install build/windows --config Release
```

The Windows install output is written to:

```text
dist/bin/llavon-ime-service.exe
```

## Run

Pass the model file and tables directory explicitly:

```powershell
dist/bin/llavon-ime-service.exe path/to/llavon-ime-llama-250m-Q4_K_M.gguf path/to/tables
```

The parent project should consume the installed executable instead of adding this
project as a CMake subdirectory.

## Linux

The service core and Fcitx frontend build with GCC 12 or newer. A minimal inference-only service build is:

```bash
cmake -S . -B build/linux -G Ninja \
  -DIMESVC_ENABLE_LLAMA=OFF \
  -DIMESVC_BUILD_TESTS=ON
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
```

Production llama.cpp inference additionally requires the `llama`, `reflectcpp`, and `utf8cpp` CMake packages.

For the Linux x86_64 trainer, use the official C++11-ABI CPU SDK:

```text
https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.7.1%2Bcpu.zip
SHA256: 63d572598c8d532128a335018913e795c1bbb32602ce378896dc8cfbb5590976
```

Configure with `IMESVC_BUILD_TRAINER=ON`, `IMESVC_LIBTORCH_PREFIX`, `IMESVC_LIBTORCH_ARCHIVE`, and the pinned
`IMESVC_LIBTORCH_SHA256`. Linux ARM64 must provide an independently verified LibTorch 2.7.1 SDK and explicit SHA256.
Install only the trainer and its LibTorch libraries with `cmake --install build/linux --component trainer`.
