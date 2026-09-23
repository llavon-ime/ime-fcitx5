# IME Unix Service

Unix socket service for Llavon IME on Linux and macOS. It provides
session-based inference to frontend processes such as `ime-fcitx5`.
Model loading, tokenization, and llama.cpp inference are provided by the
`ime-core` submodule at the repository root.

## Build

From the repository root, initialize its submodules and pass the vcpkg toolchain
from the build environment or CI:

```bash
git submodule update --init --recursive
cd ime-unix-service
cmake --preset linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux
```

The install output includes the service and the canonical tables from
`ime-core` (under the configured install prefix):

```text
bin/llavon-ime-unix-service
share/llavon-ime/tables/
```

## Run

Pass the required model file and tables directory explicitly:

```bash
dist/bin/llavon-ime-unix-service \
  --model path/to/llavon-ime-llama-250m-Q4_K_M.gguf \
  --tables path/to/tables
```

The service also accepts configuration through the `LLAVON_IME_MODEL_PATH`,
`LLAVON_IME_TABLES_DIR`, and `LLAVON_IME_UNIX_SOCKET_PATH` environment
variables. Frontends consume the installed executable rather than adding the
service as a CMake subdirectory.
