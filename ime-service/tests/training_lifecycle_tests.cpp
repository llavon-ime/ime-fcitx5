#include "engine/model_runtime.hpp"
#include "training/adapter_publisher.hpp"
#include "training/feedback_store.hpp"
#include "training/sha256.hpp"
#include "training/training_orchestrator.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#ifndef _WIN32
        path_ = std::filesystem::temp_directory_path() /
                ("imesvc-training-lifecycle-" + std::to_string(static_cast<unsigned long long>(::getpid())) + "-" + std::to_string(tick));
#else
        path_ = std::filesystem::temp_directory_path() / ("imesvc-training-lifecycle-" + std::to_string(tick));
#endif
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

imesvc::training::FeedbackEvent event(std::string id, std::int64_t created_at) {
    imesvc::training::FeedbackEvent value;
    value.event_id = std::move(id);
    value.left_context = "context";
    value.bopomofo_sequence = "ㄋㄧˇ";
    value.committed_characters = "你";
    value.predicted_top1 = "你";
    value.manually_chosen_flags = {0};
    value.signal_type = imesvc::training::FeedbackSignal::ExplicitCorrection;
    value.base_model_hash = "base";
    value.created_at_unix_seconds = created_at;
    value.eligibility = imesvc::training::FeedbackEligibility::approved_sample();
    return value;
}

std::string partition_context(std::size_t value) {
    std::string context = " ";
    for (std::size_t bit = 0; bit < 16U; ++bit) context += (value & (1U << bit)) != 0 ? "a " : "  ";
    context += "你";
    return context;
}

bool enqueue_training_and_validation(imesvc::training::FeedbackStore& store) {
    std::string training_context = partition_context(0);
    for (int index = 1; imesvc::training::FeedbackStore::deterministic_validation_member(
                            "base", training_context, "ㄋㄧˇ"); ++index) {
        training_context = partition_context(static_cast<std::size_t>(index));
    }
    std::string validation_context = partition_context(10000);
    for (int index = 1; !imesvc::training::FeedbackStore::deterministic_validation_member(
                                    "base", validation_context, "ㄋㄧˇ"); ++index) {
        validation_context = partition_context(10000U + static_cast<std::size_t>(index));
    }
    auto training = event("lifecycle-training", 1);
    training.left_context = std::move(training_context);
    auto validation = event("lifecycle-validation", 2);
    validation.left_context = std::move(validation_context);
    return store.enqueue(std::move(training)).accepted() && store.enqueue(std::move(validation)).accepted() &&
           store.flush().get().succeeded;
}

nlohmann::json adapter_tensor_metadata() {
    auto metadata = nlohmann::json::array();
    for (std::size_t layer = 0; layer < 20U; ++layer) {
        const auto prefix = "blk." + std::to_string(layer) + ".";
        const auto add = [&metadata, &prefix](const char* projection, std::int64_t input, std::int64_t output) {
            metadata.push_back({{"name", prefix + projection + ".weight.lora_a"}, {"shape", {8, input}}, {"dtype", "F32"}});
            metadata.push_back({{"name", prefix + projection + ".weight.lora_b"}, {"shape", {output, 8}}, {"dtype", "F32"}});
        };
        add("attn_q", 1024, 1024);
        add("attn_k", 1024, 1024);
        add("attn_v", 1024, 1024);
        add("attn_output", 1024, 1024);
        add("ffn_gate", 1024, 2048);
        add("ffn_up", 1024, 2048);
        add("ffn_down", 2048, 1024);
    }
    return metadata;
}

class PermissiveEnvironment final : public imesvc::training::TrainingEnvironmentProvider {
public:
    std::optional<imesvc::training::TrainingEnvironment> current_environment() override {
        return imesvc::training::TrainingEnvironment{true, 100, true, 16ULL * 1024ULL * 1024ULL * 1024ULL,
                                                      16ULL * 1024ULL * 1024ULL * 1024ULL, 0.0};
    }
};

bool publisher_test() {
    const auto fail = [](const char* reason) {
        std::cerr << "publisher test: " << reason << '\n';
        return false;
    };
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path());
    {
        std::ofstream digest_input(temporary.path() / "digest.txt", std::ios::binary);
        digest_input << "abc";
    }
    if (imesvc::training::sha256_file(temporary.path() / "digest.txt") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return fail("SHA-256");
    imesvc::training::FeedbackStoreOptions store_options;
    store_options.data_directory = temporary.path() / "data";
    imesvc::training::FeedbackStore store(store_options);
    if (!store.set_learning_enabled(true).get().succeeded || !enqueue_training_and_validation(store)) return fail("feedback setup");
    const auto snapshot = store.create_dataset_snapshot().get();
    if (!snapshot.operation.succeeded || snapshot.snapshot.validation_target_characters == 0) return fail("dataset snapshot");
    imesvc::training::TrainingRunStart run;
    run.run_id = "run-1";
    run.snapshot_id = snapshot.snapshot.snapshot_id;
    run.kind = imesvc::training::TrainingRunKind::FirstAdapter;
    run.eligible_samples = snapshot.snapshot.total_samples;
    run.eligible_target_characters = snapshot.snapshot.total_target_characters;
    run.cumulative_target_characters = store.training_accounting().get().cumulative_target_characters;
    if (!store.record_training_started(std::move(run)).get().succeeded) return fail("training run setup");

    const auto staging = temporary.path() / "staging";
    std::filesystem::create_directories(staging);
    {
        std::ofstream adapter(staging / "adapter.gguf", std::ios::binary);
        adapter << "abc";
    }
    {
        std::ofstream manifest(staging / "manifest.json");
        manifest << nlohmann::json{{"format_version", 2},
                                   {"base_model_sha256", "base"},
                                   {"base_model_revision", "base"},
                                   {"runtime_model_sha256", "runtime"},
                                   {"tokenizer_sha256", "tokenizer"},
                                   {"candidate_map_sha256", "candidates"},
                                   {"dataset_snapshot_sha256", snapshot.snapshot.sha256},
                                   {"training_code_version", "native-libtorch-lora-v1"},
                                   {"training_run_kind", "first-adapter"},
                                   {"seed", 20260713},
                                   {"rank", 8},
                                   {"alpha", 16},
                                   {"steps", 1},
                                   {"epochs", 1},
                                   {"warmup_steps", 1},
                                   {"training_loss", 0.5},
                                   {"tensor_metadata", adapter_tensor_metadata()},
                                   {"created_at", 1},
                                   {"validation_target_characters", snapshot.snapshot.validation_target_characters},
                                   {"validation_samples", snapshot.snapshot.validation_samples},
                                   {"validation_loss_before", 1.0},
                                   {"validation_loss_after", 0.5}};
    }
    imesvc::RuntimeConfig runtime_config;
    runtime_config.tables_dir = ".";
    imesvc::SharedModelRuntime runtime(runtime_config);
    bool validator_called = false;
    bool runtime_validator_called = false;
    bool post_commit_transition_reached = false;
    imesvc::training::AdapterPublisherOptions options;
    options.directory = temporary.path() / "data" / "adapters";
    options.base_model_sha256 = "base";
    options.runtime_model_sha256 = "runtime";
    options.tokenizer_sha256 = "tokenizer";
    options.candidate_map_sha256 = "candidates";
    options.validate_loadable = [&validator_called](const std::filesystem::path&) { validator_called = true; };
    options.validate_runtime = [&runtime_validator_called](const std::filesystem::path&,
                                                           const std::vector<imesvc::training::DatasetSample>& samples,
                                                           imesvc::training::TrainingRunKind) {
        runtime_validator_called = !samples.empty();
        return imesvc::training::StoreOperationResult{runtime_validator_called, runtime_validator_called ? "" : "no samples"};
    };
    options.transition_activation = [&post_commit_transition_reached](
                                        const imesvc::training::TrainingRunContext&,
                                        const std::function<imesvc::training::StoreOperationResult()>& activate) {
        const auto result = activate();
        if (result.succeeded) {
            post_commit_transition_reached = true;
            throw std::runtime_error("synthetic post-commit transition failure");
        }
        return result;
    };
    imesvc::training::AdapterPublisher publisher(store, runtime, options);
    const auto published = publisher.handle_completed_run(
        {"run-1", snapshot.snapshot.snapshot_id, imesvc::training::TrainingRunKind::FirstAdapter, staging});
    if (!published.succeeded || !validator_called || !runtime_validator_called || !post_commit_transition_reached ||
        runtime.active_adapter_version().empty()) {
        if (!published.succeeded) std::cerr << "publisher result: " << published.error << '\n';
        return fail("promotion");
    }
    const auto active = store.active_adapter().get();
    if (!active.operation.succeeded || !active.adapter ||
        active.adapter->sha256 != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        if (!active.operation.succeeded) std::cerr << "active adapter read: " << active.operation.error << '\n';
        if (active.adapter) std::cerr << "active adapter hash: " << active.adapter->sha256 << '\n';
        return fail("active adapter record");
    }
    imesvc::SharedModelRuntime restored_runtime(runtime_config);
    imesvc::training::AdapterPublisher restored_publisher(store, restored_runtime, options);
    const auto restored = restored_publisher.restore_active_adapter();
    if (!restored.succeeded || restored_runtime.active_adapter_version() != active.adapter->version) {
        return fail("active adapter restore");
    }
    if (!publisher.delete_all_artifacts().succeeded || !runtime.active_adapter_version().empty()) return fail("artifact deletion");
    return !std::filesystem::exists(temporary.path() / "data" / "adapters");
}

bool orchestrator_test() {
    TemporaryDirectory temporary;
    imesvc::training::FeedbackStoreOptions store_options;
    store_options.data_directory = temporary.path() / "data";
    imesvc::training::FeedbackStore store(store_options);
    if (!store.set_learning_enabled(true).get().succeeded || !enqueue_training_and_validation(store)) return false;
    const auto trainer = temporary.path() / "trainer.sh";
    {
        std::ofstream script(trainer);
        script << "#!/bin/sh\nexit 0\n";
    }
#ifndef _WIN32
    if (::chmod(trainer.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) return false;
#endif
    imesvc::training::TrainingOrchestratorOptions options;
    options.trainer_executable = trainer;
    options.staging_directory = temporary.path() / "staging";
    options.required_idle = std::chrono::minutes(0);
    options.evaluation_interval = std::chrono::seconds(1);
    options.minimum_free_memory_bytes = 1;
    options.minimum_free_disk_bytes = 1;
    options.thresholds.shadow_smoke_characters = 1;
    options.thresholds.first_adapter_characters = 1000;
    std::atomic_bool completed = false;
    {
        imesvc::training::TrainingOrchestrator orchestrator(
            store, std::move(options), std::make_shared<PermissiveEnvironment>(),
            [&completed](const imesvc::training::TrainingRunContext& context) {
                completed = context.kind == imesvc::training::TrainingRunKind::ShadowSmoke &&
                            !context.snapshot_id.empty() && std::filesystem::is_directory(context.staging_directory);
                return imesvc::training::StoreOperationResult{completed.load(), completed ? "" : "bad completion context"};
            });
        orchestrator.record_prediction_activity();
        orchestrator.request_evaluation();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (completed.load() && store.training_accounting().get().shadow_completed) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return false;
}

bool cancellation_test() {
    TemporaryDirectory temporary;
    imesvc::training::FeedbackStoreOptions store_options;
    store_options.data_directory = temporary.path() / "data";
    imesvc::training::FeedbackStore store(store_options);
    if (!store.set_learning_enabled(true).get().succeeded || !enqueue_training_and_validation(store)) return false;
    const auto trainer = temporary.path() / "trainer.sh";
    std::filesystem::create_directories(temporary.path());
    {
        std::ofstream script(trainer);
        script << "#!/bin/sh\nsleep 30\n";
    }
#ifndef _WIN32
    if (::chmod(trainer.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) return false;
#endif
    imesvc::training::TrainingOrchestratorOptions options;
    options.trainer_executable = trainer;
    options.staging_directory = temporary.path() / "staging";
    options.required_idle = std::chrono::minutes(0);
    options.evaluation_interval = std::chrono::seconds(1);
    options.minimum_free_memory_bytes = 1;
    options.minimum_free_disk_bytes = 1;
    options.thresholds.shadow_smoke_characters = 1;
    const auto started = std::chrono::steady_clock::now();
    {
        imesvc::training::TrainingOrchestrator orchestrator(
            store, std::move(options), std::make_shared<PermissiveEnvironment>());
        orchestrator.record_prediction_activity();
        orchestrator.request_evaluation();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline &&
               orchestrator.status().state != imesvc::training::TrainingOrchestratorState::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (orchestrator.status().state != imesvc::training::TrainingOrchestratorState::Running) return false;
    }
    if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(5)) return false;
    const auto staging = temporary.path() / "staging";
    if (std::filesystem::is_directory(staging) && !std::filesystem::is_empty(staging)) return false;
    return !store.training_accounting().get().shadow_completed;
}

}  // namespace

int main() {
    if (!publisher_test()) {
        std::cerr << "adapter publication lifecycle failed\n";
        return EXIT_FAILURE;
    }
    if (!orchestrator_test()) {
        std::cerr << "automatic training orchestration failed\n";
        return EXIT_FAILURE;
    }
    if (!cancellation_test()) {
        std::cerr << "training cancellation lifecycle failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
