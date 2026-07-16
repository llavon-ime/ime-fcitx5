#pragma once

#include "ml/model_config.hpp"

#include <torch/torch.h>

#include <filesystem>
#include <string>
#include <unordered_map>

struct llama_model;

namespace imesvc::ml {

class GgufLoraWriter final {
public:
    static void write_f32_atomic(const std::filesystem::path& output, const std::unordered_map<std::string, torch::Tensor>& tensors,
                                 const LoraConfig& config);
    static void validate_loadable(const std::filesystem::path& adapter, ::llama_model* model);
};

}  // namespace imesvc::ml
