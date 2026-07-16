#pragma once

#include "training/feedback_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace imesvc::training {

struct TrainingEnvironment {
    bool on_ac_power = false;
    int battery_percentage = -1;
    bool thermal_acceptable = false;
    std::uint64_t free_memory_bytes = 0;
    std::uint64_t free_disk_bytes = 0;
    double cpu_load_fraction = 1.0;
};

// Platform code supplies this interface; an unavailable provider blocks training by design.
class TrainingEnvironmentProvider {
public:
    virtual ~TrainingEnvironmentProvider() = default;
    [[nodiscard]] virtual std::optional<TrainingEnvironment> current_environment() = 0;
};

struct TrainingThresholds {
    std::uint64_t shadow_smoke_characters = 20000;
    std::uint64_t first_adapter_characters = 50000;
    std::uint64_t first_adapter_commits = 2000;
    std::uint64_t later_training_new_characters = 10000;
    std::uint64_t minimum_validation_characters = 5000;
    std::chrono::hours minimum_interval = std::chrono::hours(24 * 7);
};

struct TrainingOrchestratorOptions {
    std::filesystem::path trainer_executable;
    std::filesystem::path lock_path;
    std::filesystem::path heartbeat_path;
    std::filesystem::path staging_directory;
    std::vector<std::string> trainer_arguments;
    TrainingThresholds thresholds;
    std::chrono::minutes required_idle = std::chrono::minutes(10);
    std::chrono::seconds evaluation_interval = std::chrono::seconds(60);
    std::chrono::hours failure_retry_backoff = std::chrono::hours(1);
    std::uint64_t minimum_free_memory_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t minimum_free_disk_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    double maximum_cpu_load_fraction = 0.5;
    std::uint32_t trainer_intraop_threads = 1;
    std::uint32_t trainer_interop_threads = 1;
    SnapshotOptions snapshot_options;
};

enum class TrainingOrchestratorState : std::uint8_t {
    Waiting,
    Checking,
    Blocked,
    Launching,
    Running,
    Error,
    Stopped,
};

struct TrainingOrchestratorStatus {
    TrainingOrchestratorState state = TrainingOrchestratorState::Waiting;
    std::string reason;
    std::string run_id;
    std::int64_t trainer_process_id = 0;
    TrainingAccounting accounting;
};

struct TrainingRunContext {
    std::string run_id;
    std::string snapshot_id;
    TrainingRunKind kind = TrainingRunKind::ShadowSmoke;
    std::filesystem::path staging_directory;
    std::int64_t launch_heartbeat_millis = 0;
};

using TrainingCompletionHandler = std::function<StoreOperationResult(const TrainingRunContext&)>;

// Launches the optional trainer executable without linking LibTorch into the service.
class TrainingOrchestrator final {
public:
    TrainingOrchestrator(FeedbackStore& store, TrainingOrchestratorOptions options,
                         std::shared_ptr<TrainingEnvironmentProvider> environment = {},
                         TrainingCompletionHandler completion_handler = {});
    ~TrainingOrchestrator();

    TrainingOrchestrator(const TrainingOrchestrator&) = delete;
    TrainingOrchestrator& operator=(const TrainingOrchestrator&) = delete;

    // Call after every completed prediction. It is intentionally non-blocking and writes a small private heartbeat.
    void record_prediction_activity() noexcept;
    void request_evaluation() noexcept;
    [[nodiscard]] TrainingOrchestratorStatus status() const;

    [[nodiscard]] static std::int64_t current_unix_millis() noexcept;
    [[nodiscard]] static std::optional<std::int64_t> read_inference_heartbeat(const std::filesystem::path& path) noexcept;
    // Trainer code calls this after each optimizer step using the heartbeat observed before its step.
    [[nodiscard]] static bool inference_activity_since(const std::filesystem::path& path,
                                                        std::int64_t previous_heartbeat) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imesvc::training
