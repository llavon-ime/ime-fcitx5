#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace imesvc::training {

enum class FeedbackSignal : std::uint8_t {
    ExplicitCorrection = 1,
    ExplicitTop1Selection = 2,
    AcceptedPrediction = 3,
    FallbackCommit = 4,
};

// Defaults intentionally reject collection until the commit collector has performed every check.
struct FeedbackEligibility {
    bool approved = false;
    bool sensitive_context = true;
    bool cancelled = true;
    bool complete_bopomofo = false;
    bool one_to_one_alignment = false;
    bool targets_are_legal = false;
    bool excessive_unknown_tokens = true;
    bool pathological_input = true;

    static FeedbackEligibility approved_sample() noexcept;
};

struct FeedbackEvent {
    std::string event_id;
    std::string left_context;
    std::string bopomofo_sequence;
    std::string committed_characters;
    std::string predicted_top1;
    std::vector<std::uint8_t> manually_chosen_flags;
    FeedbackSignal signal_type = FeedbackSignal::FallbackCommit;
    std::string base_model_hash;
    std::string adapter_version;
    std::int64_t created_at_unix_seconds = 0;
    FeedbackEligibility eligibility;
};

struct DatasetSample {
    std::string event_id;
    std::string left_context;
    std::string bopomofo_sequence;
    std::string committed_characters;
    std::string predicted_top1;
    std::vector<std::uint8_t> manually_chosen_flags;
    FeedbackSignal signal_type = FeedbackSignal::FallbackCommit;
    std::string base_model_hash;
    std::string adapter_version;
    std::int64_t created_at_unix_seconds = 0;
    std::uint32_t target_characters = 0;
    bool validation_member = false;
};

struct RetentionPolicy {
    std::uint64_t max_target_characters = 500000;
    std::chrono::hours max_age = std::chrono::hours(24 * 365);
};

struct FeedbackStoreOptions {
    // An empty directory selects a private per-user application data directory.
    std::filesystem::path data_directory;
    std::string database_filename = "feedback.sqlite3";
    std::size_t queue_capacity = 1024;
    std::chrono::milliseconds busy_timeout = std::chrono::seconds(5);
    std::size_t retention_check_interval = 64;
    RetentionPolicy retention;
    std::size_t max_context_bytes = 256U * 1024U;
    std::size_t max_bopomofo_bytes = 4096;
    std::size_t max_target_bytes = 64U * 1024U;
    std::size_t max_target_characters = 512;
    // When set, snapshots and accounting are isolated to this verified model identity.
    std::string base_model_hash;
};

enum class FeedbackEnqueueStatus : std::uint8_t {
    Queued,
    LearningDisabled,
    QueueFull,
    Invalid,
    ShuttingDown,
};

struct FeedbackEnqueueResult {
    FeedbackEnqueueStatus status = FeedbackEnqueueStatus::Invalid;
    std::string reason;

    [[nodiscard]] bool accepted() const noexcept { return status == FeedbackEnqueueStatus::Queued; }
};

struct StoreOperationResult {
    bool succeeded = false;
    std::string error;
};

struct RetentionResult {
    StoreOperationResult operation;
    std::uint64_t removed_samples = 0;
    std::uint64_t removed_target_characters = 0;
    std::uint64_t invalidated_snapshots = 0;
};

struct SnapshotOptions {
    std::uint64_t max_target_characters = 500000;
};

struct SnapshotMetadata {
    std::string snapshot_id;
    std::string sha256;
    std::int64_t created_at_unix_seconds = 0;
    std::uint64_t total_samples = 0;
    std::uint64_t total_target_characters = 0;
    std::uint64_t training_target_characters = 0;
    std::uint64_t validation_target_characters = 0;
    std::uint64_t validation_samples = 0;
};

struct SnapshotResult {
    StoreOperationResult operation;
    SnapshotMetadata snapshot;
};

struct SnapshotLoadResult {
    StoreOperationResult operation;
    SnapshotMetadata snapshot;
    std::vector<DatasetSample> samples;
};

struct TrainingAccounting {
    bool learning_enabled = false;
    bool has_active_adapter = false;
    bool shadow_completed = false;
    std::uint64_t eligible_samples = 0;
    std::uint64_t eligible_target_characters = 0;
    std::uint64_t training_target_characters = 0;
    std::uint64_t validation_target_characters = 0;
    std::uint64_t validation_samples = 0;
    std::uint64_t eligible_target_characters_at_last_success = 0;
    std::int64_t last_training_started_at_unix_seconds = 0;
    std::int64_t last_training_completed_at_unix_seconds = 0;
    bool last_training_failed = false;
};

enum class TrainingRunKind : std::uint8_t {
    ShadowSmoke = 1,
    FirstAdapter = 2,
    Incremental = 3,
};

struct TrainingRunStart {
    std::string run_id;
    std::string snapshot_id;
    TrainingRunKind kind = TrainingRunKind::ShadowSmoke;
    std::int64_t started_at_unix_seconds = 0;
    std::uint64_t eligible_target_characters = 0;
    std::uint64_t eligible_samples = 0;
};

struct IncompleteTrainingRunsResult {
    StoreOperationResult operation;
    std::vector<TrainingRunStart> runs;
};

struct AdapterRecord {
    std::string version;
    std::string base_model_hash;
    std::string dataset_snapshot_id;
    std::string sha256;
    std::int64_t created_at_unix_seconds = 0;
    bool active = false;
};

struct AdapterLookupResult {
    StoreOperationResult operation;
    std::optional<AdapterRecord> adapter;
};

struct AdapterFeedbackStatsResult {
    StoreOperationResult operation;
    std::uint64_t eligible_target_characters = 0;
    std::uint64_t correction_target_characters = 0;
};

// SQLite-backed, single-writer feedback persistence. enqueue() never performs disk I/O.
class FeedbackStore final {
public:
    explicit FeedbackStore(FeedbackStoreOptions options = {});
    ~FeedbackStore();

    FeedbackStore(const FeedbackStore&) = delete;
    FeedbackStore& operator=(const FeedbackStore&) = delete;

    [[nodiscard]] FeedbackEnqueueResult enqueue(FeedbackEvent event);
    [[nodiscard]] std::future<StoreOperationResult> set_learning_enabled(bool enabled);
    [[nodiscard]] std::future<StoreOperationResult> delete_all_personal_data();
    [[nodiscard]] std::future<RetentionResult> apply_retention();
    [[nodiscard]] std::future<SnapshotResult> create_dataset_snapshot(SnapshotOptions options = {});
    [[nodiscard]] std::future<SnapshotLoadResult> load_dataset_snapshot(std::string snapshot_id);
    [[nodiscard]] std::future<TrainingAccounting> training_accounting();
    [[nodiscard]] std::future<StoreOperationResult> record_training_started(TrainingRunStart run);
    [[nodiscard]] std::future<IncompleteTrainingRunsResult> incomplete_training_runs();
    [[nodiscard]] std::future<StoreOperationResult> record_training_finished(std::string run_id, bool succeeded,
                                                                              std::int64_t finished_at_unix_seconds = 0);
    [[nodiscard]] std::future<StoreOperationResult> record_adapter(AdapterRecord adapter);
    [[nodiscard]] std::future<StoreOperationResult> record_adapter_and_finish_training(AdapterRecord adapter,
                                                                                        std::string run_id);
    [[nodiscard]] std::future<AdapterLookupResult> active_adapter();
    [[nodiscard]] std::future<AdapterLookupResult> latest_adapter();
    [[nodiscard]] std::future<AdapterLookupResult> previous_adapter(std::string version);
    [[nodiscard]] std::future<AdapterFeedbackStatsResult> adapter_feedback_stats(std::string version);
    [[nodiscard]] std::future<StoreOperationResult> activate_adapter(std::string version);
    [[nodiscard]] std::future<StoreOperationResult> deactivate_adapter(std::string version);

    // Resolves after every operation already accepted by the writer has completed.
    [[nodiscard]] std::future<StoreOperationResult> flush();

    [[nodiscard]] bool learning_enabled() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] const std::filesystem::path& data_directory() const noexcept;
    [[nodiscard]] const std::filesystem::path& database_path() const noexcept;

    [[nodiscard]] static bool deterministic_validation_member(const std::string& event_id) noexcept;
    [[nodiscard]] static std::filesystem::path default_data_directory();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imesvc::training
