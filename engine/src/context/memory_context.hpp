#pragma once

#include "context/accessibility_context.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace llavon::ime {

// Callbacks the memory probe needs from the engine. `inject`, `processes` and
// `sensitive` run on the engine's main thread. `remove` is invoked from the
// provider's worker thread, so the engine implementation has to marshal it
// back onto the main thread (Host::post).
struct MemoryProbeCallbacks {
    std::function<bool(std::u16string_view token)> inject;
    std::function<void(std::size_t units)> remove;
    std::function<std::vector<int>()> processes;
    std::function<bool()> sensitive;
    // Text the input method just committed (UTF-8), used to prefer the
    // document copy of the token.
    std::function<std::string()> expect_suffix;
};

// Last-resort context source: commits a private-use-area probe token at the
// caret, scans the focused client's processes with llavon-ime-memscan for that
// token, publishes the text in front of it, and removes the token again. It is
// only consulted when neither the client surrounding text nor the AT-SPI
// sample produced anything, and it is gated by the `memory_context` setting.
class MemoryContextProvider final : public AccessibilityContextProvider {
public:
    MemoryContextProvider(size_t max_code_units, MemoryProbeCallbacks callbacks,
                          std::filesystem::path helper_path);
    ~MemoryContextProvider() override;

    MemoryContextProvider(const MemoryContextProvider&) = delete;
    MemoryContextProvider& operator=(const MemoryContextProvider&) = delete;

    bool start() override;
    void stop() override;
    bool running() const noexcept override;

    // Main thread. Injects the token and schedules the scan; throttled and
    // skipped while inactive, sensitive, or backed off after failures.
    void refresh() override;

    // Number of completed helper runs (diagnostics and tests).
    std::size_t probe_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// True when this build has a probe backend for the current platform.
bool memory_context_supported();

}  // namespace llavon::ime
