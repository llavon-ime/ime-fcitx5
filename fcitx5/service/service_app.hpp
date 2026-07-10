#pragma once

#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "service/llama_backend.hpp"
#include "protocol/protocol.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace ime::fcitx5 {

class ServiceApp {
public:
    explicit ServiceApp(Config config);
    ServiceApp(Config config, std::filesystem::path table_path);

    StatusResponse handle_status() const;
    PredictResponse handle_predict(const PredictRequest& request);
    void handle_stop() noexcept;
    bool stop_requested() const noexcept;
    bool expired_idle_timeout(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

private:
    void touch();
    void ensure_llama_loaded();

    Config config_;
    std::filesystem::path table_path_;
    std::optional<FallbackEngine> fallback_;
    LlamaBackend llama_;
    bool attempted_llama_load_ = false;
    std::string backend_error_;
    std::chrono::steady_clock::time_point last_activity_;
    bool stop_requested_ = false;
};

}  // namespace ime::fcitx5
