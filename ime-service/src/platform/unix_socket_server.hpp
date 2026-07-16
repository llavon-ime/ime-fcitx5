#pragma once

#ifndef _WIN32

#include "../engine/model_runtime.hpp"
#include "../pipe/protocol.hpp"
#include "../session/session_manager.hpp"
#include "server_strategy.hpp"

#include <filesystem>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace imesvc {

namespace training {
class FeedbackStore;
class TrainingOrchestrator;
class AdapterPublisher;
struct StoreOperationResult;
}

struct LoraTrainingOptions {
    bool enabled = false;
    std::filesystem::path base_safetensors;
    std::filesystem::path trainer_executable;
    std::optional<std::filesystem::path> adapter_directory;
};

struct UnixServerOptions {
    RuntimeConfig runtime;
    SessionLimits limits;
    std::optional<std::filesystem::path> socket_path;
    std::optional<std::filesystem::path> pid_path;
    bool personal_learning_enabled = false;
    std::optional<std::filesystem::path> training_data_directory;
    LoraTrainingOptions lora_training;
};

class UnixSocketServer final : public ServerStrategy {
public:
    explicit UnixSocketServer(UnixServerOptions options);
    ~UnixSocketServer() override;

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    const char* name() const override;
    int run() override;

    static std::filesystem::path default_socket_path();
    static std::filesystem::path default_pid_path();
    [[nodiscard]] protocol::Capabilities supported_capabilities() const noexcept;

private:
    class WorkerPool;
    class Connection;

    void request_stop() noexcept;
    void accept_connections();
    void close_connections() noexcept;
    void reap_connections();
    void cleanup_endpoint() noexcept;
    void initialize_training();
    [[nodiscard]] training::StoreOperationResult delete_personal_data();
    void record_prediction_activity() noexcept;

    UnixServerOptions options_;
    std::shared_ptr<SharedModelRuntime> runtime_;
    std::unique_ptr<SessionManager> sessions_;
    std::unique_ptr<WorkerPool> workers_;
    std::unique_ptr<training::FeedbackStore> feedback_store_;
    std::unique_ptr<training::AdapterPublisher> adapter_publisher_;
    std::unique_ptr<training::TrainingOrchestrator> training_orchestrator_;
    std::string training_configuration_error_;
    int listen_fd_ = -1;
    std::filesystem::path socket_path_;
    std::filesystem::path pid_path_;
    bool endpoint_owned_ = false;
    bool pid_owned_ = false;
    std::atomic_bool stopping_{false};
    std::atomic_bool rollback_check_in_flight_{false};
    std::atomic<std::int64_t> last_prediction_activity_millis_{0};
    std::chrono::steady_clock::time_point last_rollback_check_{};

    std::mutex connections_mutex_;
    std::mutex training_mutex_;
    std::mutex activation_mutex_;
    std::mutex personal_data_mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::vector<std::thread> connection_threads_;
};

}  // namespace imesvc

#endif
