#pragma once

#include "ml/model_config.hpp"
#include "ml/training_tokenizer.hpp"
#include "training/feedback_store.hpp"

#include <torch/torch.h>

#include <cstdint>
#include <vector>

namespace imesvc::ml {

inline constexpr std::int64_t kIgnoreLabel = -100;
struct TrainingExample { torch::Tensor tokens; torch::Tensor labels; };

TrainingExample make_training_example(const training::DatasetSample& sample, const TrainingTokenizer& tokenizer,
                                      const LlamaModelConfig& config = {});
torch::Tensor causal_loss(const torch::Tensor& logits, const torch::Tensor& labels);

}  // namespace imesvc::ml
