#pragma once

#include "training/feedback_store.hpp"
#include "training/training_orchestrator.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace imesvc {
class SharedModelRuntime;
}

namespace imesvc::training {

struct AdapterPublisherOptions {
    std::filesystem::path directory;
    std::string base_model_sha256;
    std::string runtime_model_sha256;
    std::string tokenizer_sha256;
    std::string candidate_map_sha256;
    std::string training_code_version = "native-libtorch-lora-v1";
    std::uint64_t training_seed = 20260713;
    std::uint64_t minimum_validation_characters = 0;
    // The inference backend performs a real compatibility load before publication.
    std::function<void(const std::filesystem::path&)> validate_loadable;
    // Held-out samples are also evaluated through the serving backend to reject regressions.
    std::function<StoreOperationResult(const std::filesystem::path&, const std::vector<DatasetSample>&,
                                       TrainingRunKind)> validate_runtime;
    // The server provides a short prediction-arrival barrier around the final DB/runtime swap.
    std::function<StoreOperationResult(const TrainingRunContext&,
                                       const std::function<StoreOperationResult()>&)> transition_activation;
};

class AdapterPublisher final {
public:
    AdapterPublisher(FeedbackStore& store, SharedModelRuntime& runtime, AdapterPublisherOptions options);

    [[nodiscard]] StoreOperationResult restore_active_adapter();
    [[nodiscard]] StoreOperationResult handle_completed_run(const TrainingRunContext& run);
    [[nodiscard]] StoreOperationResult delete_all_artifacts();
    [[nodiscard]] StoreOperationResult evaluate_rollback();

private:
    FeedbackStore& store_;
    SharedModelRuntime& runtime_;
    AdapterPublisherOptions options_;
    std::mutex mutex_;

    void garbage_collect(std::string_view active_version);
};

}  // namespace imesvc::training
