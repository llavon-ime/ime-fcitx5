#pragma once

#include <torch/torch.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace imesvc::ml {

struct CheckpointState {
    std::unordered_map<std::string, torch::Tensor> adapter_tensors;
    torch::Tensor rng_state;
    std::uint64_t step = 0, scheduler_step = 0, epoch = 0, dataset_cursor = 0, seed = 0;
    double validation_loss_before = 0.0;
    double accumulated_training_loss = 0.0, accumulated_training_weight = 0.0;
    std::string dataset_snapshot_id, base_model_sha256, runtime_model_sha256, tokenizer_sha256,
        candidate_map_sha256, training_code_version;
    std::int64_t lora_rank = 0;
    double lora_alpha = 0.0, lora_dropout = 0.0;
};

class Checkpoint final {
public:
    static void save_atomic(const std::filesystem::path& directory, const CheckpointState& state,
                            torch::optim::Optimizer& optimizer);
    static CheckpointState load(const std::filesystem::path& directory, torch::optim::Optimizer& optimizer);
};

}  // namespace imesvc::ml
