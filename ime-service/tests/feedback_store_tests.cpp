#include "training/feedback_store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using imesvc::training::AdapterRecord;
using imesvc::training::FeedbackEligibility;
using imesvc::training::FeedbackEnqueueStatus;
using imesvc::training::FeedbackEvent;
using imesvc::training::FeedbackSignal;
using imesvc::training::FeedbackStore;
using imesvc::training::FeedbackStoreOptions;
using imesvc::training::TrainingAccounting;
using imesvc::training::TrainingRunKind;
using imesvc::training::TrainingRunStart;

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

FeedbackEvent single_character_event(std::string event_id, std::string base_model_hash = "base") {
    auto value = event();
    value.event_id = std::move(event_id);
    value.committed_characters = "\xE4\xBD\xA0";
    value.predicted_top1 = value.committed_characters;
    value.manually_chosen_flags = {0};
    value.base_model_hash = std::move(base_model_hash);
    return value;
}

bool enqueue_and_flush(FeedbackStore& store, FeedbackEvent value) {
    return store.enqueue(std::move(value)).accepted() && store.flush().get().succeeded;
}

TrainingRunStart training_run(std::string run_id, const std::string& snapshot_id, TrainingRunKind kind,
                              const TrainingAccounting& accounting, std::int64_t started_at) {
    TrainingRunStart run;
    run.run_id = std::move(run_id);
    run.snapshot_id = snapshot_id;
    run.kind = kind;
    run.started_at_unix_seconds = started_at;
    run.eligible_target_characters = accounting.eligible_target_characters;
    run.eligible_samples = accounting.eligible_samples;
    run.cumulative_target_characters = accounting.cumulative_target_characters;
    return run;
}

std::optional<std::int64_t> query_integer(const std::filesystem::path& database_path, const std::string& sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(database_path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return std::nullopt;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return std::nullopt;
    }
    std::optional<std::int64_t> result;
    if (sqlite3_step(statement) == SQLITE_ROW) result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

bool execute_sql(const std::filesystem::path& database_path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open(database_path.string().c_str(), &database) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK && error != nullptr) std::cerr << "SQLite fixture setup failed: " << error << '\n';
    sqlite3_free(error);
    sqlite3_close(database);
    return result == SQLITE_OK;
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
    if (accounting.eligible_samples != 1 || accounting.eligible_target_characters != 2 ||
        accounting.cumulative_target_characters != 2 || !accounting.learning_enabled) {
        return false;
    }

    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded || snapshot.snapshot.total_samples != 1 || snapshot.snapshot.total_target_characters != 2 ||
        snapshot.snapshot.sha256.size() != 64 || snapshot.snapshot.base_model_hash != "base" ||
        !snapshot.snapshot.payload_available) {
        return false;
    }
    const auto loaded = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    if (!loaded.operation.succeeded || loaded.samples.size() != 1 || loaded.samples.front().event_id != sample.event_id) return false;

    const auto retention = store.apply_retention().get();
    const auto purged = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    if (!retention.operation.succeeded || retention.removed_samples != 1 || retention.removed_target_characters != 2 ||
        retention.invalidated_snapshots != 1 || purged.operation.succeeded ||
        purged.operation.error.find("payload is unavailable") == std::string::npos || purged.snapshot.payload_available ||
        purged.snapshot.snapshot_id != snapshot.snapshot.snapshot_id ||
        query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshots") != 1 ||
        query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshot_samples") != 0) {
        return false;
    }
    const auto retained = store.training_accounting().get();
    if (retained.eligible_samples != 0 || retained.cumulative_target_characters != 2) return false;
    if (!store.delete_all_personal_data().get().succeeded || store.learning_enabled()) return false;
    if (store.training_accounting().get().cumulative_target_characters != 0) return false;

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

bool retention_watermark_churn_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    options.retention.max_target_characters = 2;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("churn-a")) ||
        !enqueue_and_flush(store, single_character_event("churn-b"))) {
        return false;
    }

    const auto at_cap = store.training_accounting().get();
    const auto snapshot = store.create_dataset_snapshot().get();
    if (at_cap.eligible_target_characters != 2 || at_cap.cumulative_target_characters != 2 ||
        !snapshot.operation.succeeded ||
        !store.record_training_started(training_run("churn-run", snapshot.snapshot.snapshot_id,
                                                    TrainingRunKind::Incremental, at_cap, 10))
             .get()
             .succeeded ||
        !store.record_training_finished("churn-run", true, 20).get().succeeded) {
        return false;
    }

    if (!store.enqueue(single_character_event("churn-c")).accepted() ||
        !store.enqueue(single_character_event("churn-d")).accepted() ||
        !store.enqueue(single_character_event("churn-d")).accepted() || !store.flush().get().succeeded) {
        return false;
    }
    const auto retention = store.apply_retention().get();
    const auto accounting = store.training_accounting().get();
    const auto new_characters = accounting.cumulative_target_characters >=
                                        accounting.cumulative_target_characters_at_last_success
                                    ? accounting.cumulative_target_characters -
                                          accounting.cumulative_target_characters_at_last_success
                                    : 0;
    return retention.operation.succeeded && retention.removed_samples == 2 &&
           retention.removed_target_characters == 2 && accounting.eligible_target_characters == 2 &&
           accounting.cumulative_target_characters == 4 &&
           accounting.cumulative_target_characters_at_last_success == 2 && new_characters >= 2;
}

bool completed_snapshot_provenance_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    options.retention.max_target_characters = 0;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("completed-snapshot"))) {
        return false;
    }
    const auto accounting = store.training_accounting().get();
    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded ||
        !store.record_training_started(training_run("shadow-run", snapshot.snapshot.snapshot_id,
                                                    TrainingRunKind::ShadowSmoke, accounting, 10))
             .get()
             .succeeded ||
        !store.record_training_finished("shadow-run", true, 20).get().succeeded) {
        return false;
    }

    AdapterRecord adapter;
    adapter.version = "adapter-1";
    adapter.base_model_hash = "base";
    adapter.dataset_snapshot_id = snapshot.snapshot.snapshot_id;
    adapter.sha256 = "adapter-digest";
    adapter.created_at_unix_seconds = 25;
    adapter.active = true;
    if (!store.record_adapter(adapter).get().succeeded ||
        !store.record_training_started(training_run("failed-run", snapshot.snapshot.snapshot_id,
                                                    TrainingRunKind::Incremental, accounting, 30))
             .get()
             .succeeded ||
        !store.record_training_finished("failed-run", false, 40).get().succeeded) {
        return false;
    }

    const auto before = store.training_accounting().get();
    const auto retention = store.apply_retention().get();
    const auto purged = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    const auto after = store.training_accounting().get();
    const auto active = store.active_adapter().get();
    return before.has_active_adapter && before.shadow_completed && before.last_training_failed &&
           before.last_training_started_at_unix_seconds == 30 &&
           before.last_training_completed_at_unix_seconds == 40 &&
           before.cumulative_target_characters_at_last_success == 1 && retention.operation.succeeded &&
           retention.removed_samples == 1 && retention.removed_target_characters == 1 &&
           retention.invalidated_snapshots == 1 && !purged.operation.succeeded &&
           purged.operation.error.find("payload is unavailable") != std::string::npos &&
           purged.snapshot.snapshot_id == snapshot.snapshot.snapshot_id && purged.snapshot.base_model_hash == "base" &&
           !purged.snapshot.payload_available && after.has_active_adapter && after.shadow_completed &&
           after.last_training_failed && after.last_training_started_at_unix_seconds == 30 &&
           after.last_training_completed_at_unix_seconds == 40 &&
           after.cumulative_target_characters_at_last_success == 1 && active.operation.succeeded && active.adapter &&
           active.adapter->version == adapter.version &&
           active.adapter->dataset_snapshot_id == snapshot.snapshot.snapshot_id &&
           query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshots") == 1 &&
           query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshot_samples") == 0;
}

bool incomplete_snapshot_payload_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    options.retention.max_target_characters = 0;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("incomplete-snapshot"))) {
        return false;
    }
    const auto accounting = store.training_accounting().get();
    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded ||
        !store.record_training_started(training_run("incomplete-run", snapshot.snapshot.snapshot_id,
                                                     TrainingRunKind::Incremental, accounting, 10))
             .get()
             .succeeded) {
        return false;
    }
    if (store.record_training_started(training_run("incomplete-run", snapshot.snapshot.snapshot_id,
                                                   TrainingRunKind::Incremental, accounting, 11))
            .get()
            .succeeded) {
        return false;
    }

    const auto protected_retention = store.apply_retention().get();
    const auto protected_snapshot = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    if (!protected_retention.operation.succeeded || protected_retention.removed_samples != 1 ||
        protected_retention.invalidated_snapshots != 0 || !protected_snapshot.operation.succeeded ||
        !protected_snapshot.snapshot.payload_available || protected_snapshot.samples.size() != 1 ||
        query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshot_samples") != 1) {
        return false;
    }

    if (!store.record_training_finished("incomplete-run", false, 20).get().succeeded) return false;
    const auto eventual_retention = store.apply_retention().get();
    const auto purged = store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get();
    return eventual_retention.operation.succeeded && eventual_retention.removed_samples == 0 &&
           eventual_retention.invalidated_snapshots == 1 && !purged.operation.succeeded &&
           purged.operation.error.find("payload is unavailable") != std::string::npos &&
           query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshot_samples") == 0;
}

bool adapter_lineage_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("lineage-sample"))) {
        return false;
    }
    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded) return false;
    const auto add_adapter = [&store, &snapshot](std::string version) {
        AdapterRecord adapter;
        adapter.version = std::move(version);
        adapter.base_model_hash = "base";
        adapter.dataset_snapshot_id = snapshot.snapshot.snapshot_id;
        adapter.sha256 = "digest-" + adapter.version;
        adapter.active = true;
        return store.record_adapter(std::move(adapter)).get().succeeded;
    };

    if (!add_adapter("adapter-a") || !add_adapter("adapter-b") ||
        !store.rollback_adapter("adapter-b", "adapter-a").get().succeeded ||
        store.activate_adapter("adapter-b").get().succeeded) {
        return false;
    }
    const auto latest_after_rollback = store.latest_adapter().get();
    if (!latest_after_rollback.operation.succeeded || !latest_after_rollback.adapter ||
        latest_after_rollback.adapter->version != "adapter-a" || !add_adapter("adapter-d")) {
        return false;
    }
    const auto previous = store.previous_adapter("adapter-d").get();
    const auto lineage = store.adapter_lineage("adapter-d").get();
    if (!previous.operation.succeeded || !previous.adapter || previous.adapter->version != "adapter-a" ||
        !lineage.operation.succeeded || lineage.adapters.size() != 2 ||
        lineage.adapters[0].version != "adapter-d" || lineage.adapters[1].version != "adapter-a" ||
        query_integer(store.database_path(), "SELECT rejected FROM adapters WHERE version = 'adapter-b'") != 1) {
        return false;
    }
    AdapterRecord duplicate;
    duplicate.version = "adapter-d";
    duplicate.base_model_hash = "base";
    duplicate.dataset_snapshot_id = snapshot.snapshot.snapshot_id;
    duplicate.sha256 = "replacement-digest";
    duplicate.active = true;
    if (store.record_adapter(std::move(duplicate)).get().succeeded) return false;
    AdapterRecord unrelated;
    unrelated.version = "adapter-unrelated";
    unrelated.base_model_hash = "base";
    unrelated.dataset_snapshot_id = snapshot.snapshot.snapshot_id;
    unrelated.sha256 = "unrelated-digest";
    if (!store.record_adapter(std::move(unrelated)).get().succeeded ||
        store.rollback_adapter("adapter-d", "adapter-unrelated").get().succeeded) {
        return false;
    }
    AdapterRecord missing_parent;
    missing_parent.version = "adapter-missing-parent";
    missing_parent.base_model_hash = "base";
    missing_parent.dataset_snapshot_id = snapshot.snapshot.snapshot_id;
    missing_parent.sha256 = "missing-parent-digest";
    missing_parent.parent_version = "does-not-exist";
    if (store.record_adapter(std::move(missing_parent)).get().succeeded) return false;
    if (!store.rollback_adapter("adapter-d", "adapter-a").get().succeeded) return false;
    const auto active = store.active_adapter().get();
    return active.operation.succeeded && active.adapter && active.adapter->version == "adapter-a" &&
           query_integer(store.database_path(), "SELECT rejected FROM adapters WHERE version = 'adapter-d'") == 1;
}

bool grouped_validation_split_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    FeedbackStore store(options);
    auto first = single_character_event("duplicate-prompt-a");
    first.left_context = "X9";
    auto second = single_character_event("duplicate-prompt-b");
    second.left_context = "x9";
    if (!store.set_learning_enabled(true).get().succeeded || !enqueue_and_flush(store, std::move(first)) ||
        !enqueue_and_flush(store, std::move(second))) {
        return false;
    }
    return query_integer(store.database_path(),
                         "SELECT COUNT(DISTINCT validation_member) FROM samples "
                         "WHERE event_id IN ('duplicate-prompt-a', 'duplicate-prompt-b')") == 1;
}

bool mixed_model_snapshot_rejection_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.retention_check_interval = 0;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("mixed-a", "base-a")) ||
        !enqueue_and_flush(store, single_character_event("mixed-b", "base-b"))) {
        return false;
    }
    const auto snapshot = store.create_dataset_snapshot().get();
    return !snapshot.operation.succeeded &&
           snapshot.operation.error.find("multiple base models") != std::string::npos &&
           store.training_accounting().get().cumulative_target_characters == 2 &&
           query_integer(store.database_path(), "SELECT COUNT(*) FROM dataset_snapshots") == 0;
}

bool discard_unreferenced_snapshot_test() {
    TemporaryDirectory temporary;
    FeedbackStoreOptions options;
    options.data_directory = temporary.path();
    options.base_model_hash = "base";
    options.retention_check_interval = 0;
    FeedbackStore store(options);
    if (!store.set_learning_enabled(true).get().succeeded ||
        !enqueue_and_flush(store, single_character_event("discard-snapshot"))) {
        return false;
    }
    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded ||
        !store.discard_dataset_snapshot(snapshot.snapshot.snapshot_id).get().succeeded) {
        return false;
    }
    return !store.load_dataset_snapshot(snapshot.snapshot.snapshot_id).get().operation.succeeded &&
           !store.discard_dataset_snapshot(snapshot.snapshot.snapshot_id).get().succeeded;
}

bool schema_v2_migration_test() {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path());
    const auto database_path = temporary.path() / "feedback.sqlite3";
    if (!execute_sql(database_path, R"sql(
        CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);
        CREATE TABLE samples(
            event_id TEXT PRIMARY KEY, left_context TEXT NOT NULL, bopomofo_sequence TEXT NOT NULL,
            committed_characters TEXT NOT NULL, predicted_top1 TEXT NOT NULL, manually_chosen_flags BLOB NOT NULL,
            signal_type INTEGER NOT NULL, base_model_hash TEXT NOT NULL, adapter_version TEXT NOT NULL,
            created_at INTEGER NOT NULL, target_characters INTEGER NOT NULL CHECK(target_characters >= 0),
            validation_member INTEGER NOT NULL CHECK(validation_member IN (0, 1)),
            correction_characters INTEGER NOT NULL DEFAULT 0
                CHECK(correction_characters >= 0 AND correction_characters <= target_characters));
        CREATE TABLE dataset_snapshots(
            snapshot_id TEXT PRIMARY KEY, sha256 TEXT NOT NULL, created_at INTEGER NOT NULL,
            total_samples INTEGER NOT NULL, total_target_characters INTEGER NOT NULL,
            training_target_characters INTEGER NOT NULL, validation_target_characters INTEGER NOT NULL,
            validation_samples INTEGER NOT NULL);
        CREATE TABLE dataset_snapshot_samples(
            snapshot_id TEXT NOT NULL REFERENCES dataset_snapshots(snapshot_id) ON DELETE CASCADE,
            ordinal INTEGER NOT NULL, event_id TEXT NOT NULL, left_context TEXT NOT NULL,
            bopomofo_sequence TEXT NOT NULL, committed_characters TEXT NOT NULL, predicted_top1 TEXT NOT NULL,
            manually_chosen_flags BLOB NOT NULL, signal_type INTEGER NOT NULL, base_model_hash TEXT NOT NULL,
            adapter_version TEXT NOT NULL, created_at INTEGER NOT NULL, target_characters INTEGER NOT NULL,
            validation_member INTEGER NOT NULL, PRIMARY KEY(snapshot_id, ordinal));
        CREATE TABLE training_runs(
            run_id TEXT PRIMARY KEY, snapshot_id TEXT NOT NULL, kind INTEGER NOT NULL, started_at INTEGER NOT NULL,
            completed_at INTEGER, succeeded INTEGER CHECK(succeeded IN (0, 1)),
            eligible_target_characters INTEGER NOT NULL, eligible_samples INTEGER NOT NULL);
        CREATE TABLE adapters(
            version TEXT PRIMARY KEY, base_model_hash TEXT NOT NULL, dataset_snapshot_id TEXT NOT NULL,
            sha256 TEXT NOT NULL, created_at INTEGER NOT NULL, active INTEGER NOT NULL CHECK(active IN (0, 1)));
        CREATE TABLE learning_state(
            id INTEGER PRIMARY KEY CHECK(id = 1), learning_enabled INTEGER NOT NULL CHECK(learning_enabled IN (0, 1)));
        INSERT INTO schema_migrations VALUES(1, 1), (2, 2);
        INSERT INTO learning_state VALUES(1, 1);
        INSERT INTO samples VALUES(
            'legacy-a', 'context', 'bo', 'ab', 'ab', X'0000', 1, 'base-a', '', 1, 2, 0, 0);
        INSERT INTO samples VALUES(
            'legacy-b', 'context', 'bo', 'abc', 'abc', X'000000', 1, 'base-b', '', 2, 3, 0, 0);
        INSERT INTO samples VALUES(
            'legacy-later', 'context', 'bo', 'a', 'a', X'00', 1, 'base-a', '', 30, 1, 1, 0);
        INSERT INTO dataset_snapshots VALUES('legacy-snapshot', 'legacy-sha', 3, 1, 2, 2, 0, 0);
        INSERT INTO dataset_snapshots VALUES('legacy-mixed-snapshot', 'mixed-sha', 4, 2, 5, 5, 0, 0);
        INSERT INTO dataset_snapshot_samples VALUES(
            'legacy-snapshot', 0, 'legacy-a', 'context', 'bo', 'ab', 'ab', X'0000', 1, 'base-a', '', 1, 2, 0);
        INSERT INTO dataset_snapshot_samples VALUES(
            'legacy-mixed-snapshot', 0, 'legacy-a', 'context', 'bo', 'ab', 'ab', X'0000', 1, 'base-a', '', 1, 2, 0);
        INSERT INTO dataset_snapshot_samples VALUES(
            'legacy-mixed-snapshot', 1, 'legacy-b', 'context', 'bo', 'abc', 'abc', X'000000', 1, 'base-b', '', 2, 3, 0);
        INSERT INTO training_runs VALUES('legacy-run', 'legacy-snapshot', 1, 10, 20, 1, 2, 1);
        INSERT INTO training_runs VALUES('legacy-mixed-run', 'legacy-mixed-snapshot', 2, 8, NULL, NULL, 5, 2);
        INSERT INTO adapters VALUES('legacy-adapter-a', 'base-a', 'legacy-snapshot', 'sha-a', 5, 0);
        INSERT INTO adapters VALUES('legacy-adapter-b', 'base-a', 'legacy-snapshot', 'sha-b', 6, 0);
        INSERT INTO adapters VALUES('legacy-adapter-current', 'base-a', 'legacy-snapshot', 'sha-current', 7, 1);
        PRAGMA user_version = 2;
    )sql")) {
        return false;
    }

    {
        FeedbackStoreOptions options;
        options.data_directory = temporary.path();
        options.base_model_hash = "base-a";
        options.retention_check_interval = 0;
        FeedbackStore store(options);
        const auto migrated = store.training_accounting().get();
        if (!store.available() || !store.learning_enabled() || migrated.eligible_samples != 2 ||
            migrated.eligible_target_characters != 3 || migrated.cumulative_target_characters != 3 ||
            !migrated.shadow_completed || migrated.cumulative_target_characters_at_last_success != 2 ||
            migrated.last_training_started_at_unix_seconds != 10 ||
            migrated.last_training_completed_at_unix_seconds != 20 ||
            !enqueue_and_flush(store, single_character_event("legacy-new", "base-a")) ||
            store.training_accounting().get().cumulative_target_characters != 4) {
            return false;
        }
    }

    if (query_integer(database_path, "PRAGMA user_version") != 5 ||
        query_integer(database_path,
                      "SELECT COUNT(*) FROM dataset_snapshots WHERE base_model_hash = 'base-a' AND payload_available = 0") != 1 ||
        query_integer(database_path,
                      "SELECT COUNT(DISTINCT validation_member) FROM samples WHERE event_id IN ('legacy-a', 'legacy-later')") != 1 ||
        query_integer(database_path, "SELECT COUNT(*) FROM adapters WHERE parent_version = ''") != 3 ||
        query_integer(database_path,
                      "SELECT COUNT(*) FROM dataset_snapshots WHERE snapshot_id = 'legacy-mixed-snapshot' "
                      "AND base_model_hash = '' AND payload_available = 0") != 1 ||
        query_integer(database_path,
                      "SELECT COUNT(*) FROM dataset_snapshot_samples WHERE snapshot_id = 'legacy-mixed-snapshot'") != 0 ||
        query_integer(database_path, "SELECT COUNT(*) FROM dataset_snapshot_samples") != 0 ||
        query_integer(database_path,
                      "SELECT COUNT(*) FROM training_runs WHERE run_id = 'legacy-mixed-run' "
                      "AND completed_at IS NOT NULL AND succeeded = 0") != 1) {
        return false;
    }

    {
        FeedbackStoreOptions options;
        options.data_directory = temporary.path();
        options.retention_check_interval = 0;
        FeedbackStore store(options);
        const auto global = store.training_accounting().get();
        if (global.eligible_target_characters != 7 || global.cumulative_target_characters != 7 ||
            !global.shadow_completed || global.cumulative_target_characters_at_last_success != 2 ||
            !store.delete_all_personal_data().get().succeeded ||
            store.training_accounting().get().cumulative_target_characters != 0) {
            return false;
        }
    }
    return query_integer(database_path, "SELECT COUNT(*) FROM base_model_progress") == 0;
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

bool stale_base_model_run_cleanup_test() {
    TemporaryDirectory temporary;
    std::string snapshot_id;
    {
        FeedbackStoreOptions options;
        options.data_directory = temporary.path();
        options.base_model_hash = "old-base";
        options.retention_check_interval = 0;
        FeedbackStore store(options);
        if (!store.set_learning_enabled(true).get().succeeded ||
            !enqueue_and_flush(store, single_character_event("old-base-event", "old-base"))) {
            return false;
        }
        const auto accounting = store.training_accounting().get();
        const auto snapshot = store.create_dataset_snapshot().get();
        if (!snapshot.operation.succeeded ||
            !store.record_training_started(training_run("old-base-run", snapshot.snapshot.snapshot_id,
                                                         TrainingRunKind::Incremental, accounting, 10))
                 .get()
                 .succeeded) {
            return false;
        }
        snapshot_id = snapshot.snapshot.snapshot_id;
    }
    {
        FeedbackStoreOptions options;
        options.data_directory = temporary.path();
        options.base_model_hash = "new-base";
        options.retention_check_interval = 0;
        FeedbackStore store(options);
        if (!store.available() || !store.incomplete_training_runs().get().runs.empty()) return false;
    }
    return query_integer(temporary.path() / "feedback.sqlite3",
                         "SELECT COUNT(*) FROM training_runs WHERE run_id = 'old-base-run' "
                         "AND completed_at IS NOT NULL AND succeeded = 0") == 1 &&
           query_integer(temporary.path() / "feedback.sqlite3",
                         "SELECT COUNT(*) FROM dataset_snapshots WHERE snapshot_id = '" + snapshot_id +
                             "' AND payload_available = 0") == 1 &&
           query_integer(temporary.path() / "feedback.sqlite3",
                         "SELECT COUNT(*) FROM dataset_snapshot_samples WHERE snapshot_id = '" + snapshot_id + "'") == 0;
}

bool unsafe_unavailable_store_deletion_test() {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "data");
    const auto outside = temporary.path() / "outside.db";
    {
        std::ofstream output(outside);
        output << "must remain";
    }
    FeedbackStoreOptions options;
    options.data_directory = temporary.path() / "data";
    options.database_filename = "../outside.db";
    FeedbackStore store(options);
    return !store.available() && !store.delete_all_personal_data().get().succeeded &&
           std::filesystem::is_regular_file(outside);
}

}  // namespace

int main() {
    if (!feedback_store_test()) std::cerr << "basic feedback store test failed\n";
    else if (!retention_watermark_churn_test()) std::cerr << "retention watermark churn test failed\n";
    else if (!completed_snapshot_provenance_test()) std::cerr << "completed snapshot provenance test failed\n";
    else if (!incomplete_snapshot_payload_test()) std::cerr << "incomplete snapshot payload test failed\n";
    else if (!adapter_lineage_test()) std::cerr << "adapter lineage test failed\n";
    else if (!grouped_validation_split_test()) std::cerr << "grouped validation split test failed\n";
    else if (!mixed_model_snapshot_rejection_test()) std::cerr << "mixed-model snapshot rejection test failed\n";
    else if (!discard_unreferenced_snapshot_test()) std::cerr << "discard unreferenced snapshot test failed\n";
    else if (!schema_v2_migration_test()) std::cerr << "schema v2 migration test failed\n";
    else if (!unavailable_store_deletion_test()) std::cerr << "unavailable store deletion test failed\n";
    else if (!stale_base_model_run_cleanup_test()) std::cerr << "stale base-model run cleanup test failed\n";
    else if (!unsafe_unavailable_store_deletion_test()) std::cerr << "unsafe unavailable store deletion test failed\n";
    else return EXIT_SUCCESS;
    return EXIT_FAILURE;
}
