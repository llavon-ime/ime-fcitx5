# IME Core

Cross-platform static C++ inference library for Llavon IME. This project owns model loading,
tokenization, candidate masking, llama.cpp inference, and per-client inference
sessions. It contains no service IPC, process startup, or operating-system
specific path discovery.

## Build

Pass a vcpkg toolchain from the caller:

```powershell
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset windows
cmake --install build/windows --config Release
```

The installed CMake package exports the static target `ime-core::ime-core`.

## Core data

`CoreConfig` requires a GGUF model path and a tables directory. The installed
reference tables are placed under `share/ime-core/tables`.

Inference-device selection is supplied as data through `CoreConfig`.
`enumerate_inference_devices()` reports devices exposed by the loaded ggml
backends without loading a model. The core does not locate or parse application
settings files.

## Logging

`CoreConfig::logger` accepts an optional implementation of the platform-neutral
`Logger` interface. `Logger::log(std::string)` handles messages that already
exist, while `Logger::log(MessageFactory)` defers expensive formatting. A
logger must never evaluate a message factory on the calling thread, and must
not evaluate it at all when logging is disabled or the message is rejected.
The core contains no logging thread, queue, pipe, or platform transport.
