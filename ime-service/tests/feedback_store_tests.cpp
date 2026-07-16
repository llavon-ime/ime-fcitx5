#include "training/feedback_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using imesvc::training::FeedbackEligibility;
using imesvc::training::FeedbackEnqueueStatus;
using imesvc::training::FeedbackEvent;
using imesvc::training::FeedbackSignal;
using imesvc::training::FeedbackStore;
using imesvc::training::FeedbackStoreOptions;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#ifndef _WIN32
        path_ = std::filesystem::temp_directory_path() /
                ("imesvc-feedback-store-" + std::to_string(static_cast<unsigned long long>(::getpid())) + "-" + std::to_string(tick));
#else
        path_ = std::filesystem::temp_directory_path() / ("imesvc-feedback-store-" + std::to_string(tick));
#endif
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

FeedbackEvent event() {
    FeedbackEvent value;
    value.event_id = "event-1";
    value.left_context = "context";
    value.bopomofo_sequence = "\xE3\x84\x8B\xE3\x84\xA7";
    value.committed_characters = "\xE4\xBD\xA0\xE5\xA5\xBD";
    value.predicted_top1 = value.committed_characters;
    value.manually_chosen_flags = {0, 1};
    value.signal_type = FeedbackSignal::ExplicitCorrection;
    value.base_model_hash = "base";
    value.eligibility = FeedbackEligibility::approved_sample();
    return value;
}

bool feedback_store_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.queue_capacity = 16;
    options.retention_check_interval = 0;
    options.retention.max_target_characters = 1;
    FeedbackStore store(options);

    if (store.learning_enabled()) return false;
    if (store.enqueue(event()).status != FeedbackEnqueueStatus::LearningDisabled) return false;
    if (!store.set_learning_enabled(true).get().succeeded || !store.learning_enabled()) return false;

    const auto sample = event();
    if (store.enqueue(sample).status != FeedbackEnqueueStatus::Queued || store.enqueue(sample).status != FeedbackEnqueueStatus::Queued ||
        !store.flush().get().succeeded) {
        return false;
    }
    const auto accounting = store.training_accounting().get();
    if (accounting.eligible_samples != 1 || accounting.eligible_target_characters != 2 || !accounting.learning_enabled) return false;

    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded || snapshot.snapshot.total_samples != 1 || snapshot.snapshot.total_target_characters != 2 ||
        snapshot.snapshot.sha256.size() != 64) {
        return false;
    }
    const auto loaded = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    if (!loaded.operation.succeeded || loaded.samples.size() != 1 || loaded.samples.front().event_id != sample.event_id) return false;

    const auto retention = store.apply_retention().get();
    if (!retention.operation.succeeded || retention.removed_samples != 1 || retention.removed_target_characters != 2 ||
        retention.invalidated_snapshots != 1 || store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get().operation.succeeded) {
        return false;
    }
    if (store.training_accounting().get().eligible_samples != 0) return false;
    if (!store.delete_all_personal_data().get().succeeded || store.learning_enabled()) return false;

#ifndef _WIN32
    struct stat directory_information {};
    struct stat database_information {};
    if (::stat(store.data_directory().c_str(), &directory_information) != 0 ||
        ::stat(store.database_path().c_str(), &database_information) != 0 ||
        (directory_information.st_mode & 0777) != 0700 || (database_information.st_mode & 0777) != 0600) {
        return false;
    }
#endif
    return true;
}

bool unavailable_store_deletion_test() {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path());
    {
        std::ofstream corrupt(temporary.path() / "feedback.sqlite3", std::ios::binary);
        corrupt << "not a sqlite database";
    }
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    FeedbackStore store(options);
    if (store.available()) return false;
    const auto deleted = store.delete_all_personal_data().get();
    return deleted.succeeded && !std::filesystem::exists(store.database_path()) && !store.learning_enabled();
}

}  // namespace

int main() { return feedback_store_test() && unavailable_store_deletion_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
