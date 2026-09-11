#pragma once

#include <ime-core/core.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace ime::unix_service {

class StderrLogger;

struct RuntimeConfig {
    std::filesystem::path model_path;
    std::filesystem::path tables_dir;
    std::uint32_t context_length = 512;
    std::uint32_t threads = 8;
    int gpu_layers = -2;
};

class CoreRuntime final {
public:
    explicit CoreRuntime(RuntimeConfig config);

    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;

    void validate_configuration() const;
    std::unique_ptr<llavon::ime::core::Session> create_session();
    bool loaded() const noexcept;

private:
    void ensure_loaded();

    RuntimeConfig config_;
    std::once_flag load_once_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<llavon::ime::core::Core> core_;
    std::shared_ptr<StderrLogger> logger_;
};

}  // namespace ime::unix_service
