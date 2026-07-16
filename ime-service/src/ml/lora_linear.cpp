#include "ml/lora_linear.hpp"

#include <stdexcept>

namespace imesvc::ml {

LoraLinearImpl::LoraLinearImpl(std::int64_t input_features, std::int64_t output_features, LoraConfig config)
    : scale_(config.alpha / static_cast<double>(config.rank)), dropout_(config.dropout) {
    config.validate();
    weight = register_parameter("weight", torch::empty({output_features, input_features}, torch::kFloat32), false);
    lora_a = register_parameter("lora_a", torch::randn({config.rank, input_features}, torch::kFloat32) * 0.01);
    lora_b = register_parameter("lora_b", torch::zeros({output_features, config.rank}, torch::kFloat32));
}

torch::Tensor LoraLinearImpl::forward(const torch::Tensor& input) {
    namespace F = torch::nn::functional;
    auto adapter_input = input;
    if (is_training() && dropout_ > 0.0) adapter_input = torch::dropout(adapter_input, dropout_, true);
    return F::linear(input, weight) + F::linear(F::linear(adapter_input, lora_a), lora_b) * scale_;
}

std::vector<torch::Tensor> LoraLinearImpl::adapter_parameters() const { return {lora_a, lora_b}; }

bool LoraLinearImpl::base_has_gradients() const { return weight.grad().defined(); }

void LoraLinearImpl::assert_base_is_frozen() const {
    if (weight.requires_grad() || base_has_gradients()) throw std::logic_error("a frozen base projection received gradients");
}

}  // namespace imesvc::ml
