#include "service/service_app.hpp"

#include <cstdlib>
#include <utility>

namespace ime::fcitx5 {

namespace {

const char* non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

std::filesystem::path default_table_path() {
    if (const char* override = non_empty_env("IME_FCITX5_TABLE_PATH")) return override;
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_path =
            std::filesystem::path(home) / "Library" / "fcitx5" / "share" / "llavon-ime" / "tables" /
            "bopomofo_char.json";
        if (std::filesystem::exists(user_path)) return user_path;
    }
#endif
#ifdef IME_FCITX5_SOURCE_DIR
    const auto source_path = std::filesystem::path(IME_FCITX5_SOURCE_DIR) / ".." / "tables" / "bopomofo_char.json";
    if (std::filesystem::exists(source_path)) return source_path;
#endif
#ifdef IME_FCITX5_INSTALLED_TABLE_PATH
    const auto installed_path = std::filesystem::path(IME_FCITX5_INSTALLED_TABLE_PATH);
    if (std::filesystem::exists(installed_path)) return installed_path;
    return installed_path;
#endif
    return "/usr/share/llavon-ime/tables/bopomofo_char.json";
}

}  // namespace

ServiceApp::ServiceApp(Config config) : ServiceApp(std::move(config), default_table_path()) {}

ServiceApp::ServiceApp(Config config, std::filesystem::path table_path)
    : config_(std::move(config)),
      table_path_(std::move(table_path)),
      last_activity_(std::chrono::steady_clock::now()) {}

StatusResponse ServiceApp::handle_status() const {
    StatusResponse status;
    status.running = !stop_requested_;
    status.model_loaded = llama_.ready();
    status.backend = llama_.ready() ? "llama.cpp" : "fallback";
    status.model_path = config_.model_path;
    if (config_.model_path.empty()) {
        status.error = "model not configured";
    } else if (!llama_.ready()) {
        status.error = backend_error_.empty() ? "model not loaded" : backend_error_;
    }
    return status;
}

PredictResponse ServiceApp::handle_predict(const PredictRequest& request) {
    touch();
    if (!config_.model_path.empty()) {
        ensure_llama_loaded();
        if (llama_.ready()) {
            try {
                return llama_.predict(request);
            } catch (const std::exception& error) {
                backend_error_ = error.what();
            }
        }
    }

    if (!fallback_) fallback_.emplace(table_path_);

    PredictResponse response;
    response.candidates.reserve(request.padding.size());
    for (const auto& entry : request.padding) {
        if (entry.chosen && entry.chosen_char != 0) {
            response.candidates.push_back({entry.chosen_char});
        } else {
            CompositionBuffer buffer;
            for (char16_t symbol : entry.bopomofo) buffer.add_bopomofo(symbol);
            const auto predictions = fallback_->predict(buffer);
            if (predictions.empty()) {
                response.candidates.push_back({});
            } else {
                response.candidates.push_back(predictions.back().candidates);
            }
        }
    }
    return response;
}

void ServiceApp::handle_stop() noexcept {
    stop_requested_ = true;
}

bool ServiceApp::stop_requested() const noexcept {
    return stop_requested_;
}

bool ServiceApp::expired_idle_timeout(std::chrono::steady_clock::time_point now) const {
    return now - last_activity_ >= std::chrono::seconds(config_.idle_timeout_seconds);
}

void ServiceApp::touch() {
    last_activity_ = std::chrono::steady_clock::now();
}

void ServiceApp::ensure_llama_loaded() {
    if (attempted_llama_load_) return;
    attempted_llama_load_ = true;
    try {
        llama_.load(config_);
        backend_error_.clear();
    } catch (const std::exception& error) {
        backend_error_ = error.what();
    }
}

}  // namespace ime::fcitx5
