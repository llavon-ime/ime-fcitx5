#pragma once

#include "ml/model_config.hpp"

#include <torch/torch.h>

#include <string>
#include <vector>

namespace imesvc::ml {

class LoraLinearImpl final : public torch::nn::Module {
public:
    LoraLinearImpl(std::int64_t input_features, std::int64_t output_features, LoraConfig config);

    torch::Tensor forward(const torch::Tensor& input);
    [[nodiscard]] std::vector<torch::Tensor> adapter_parameters() const;
    [[nodiscard]] bool base_has_gradients() const;
    void assert_base_is_frozen() const;

    torch::Tensor weight;
    torch::Tensor lora_a;
    torch::Tensor lora_b;

private:
    double scale_;
    double dropout_;
};
TORCH_MODULE(LoraLinear);

}  // namespace imesvc::ml
