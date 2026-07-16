#pragma once

#include <torch/torch.h>

#include <cstdint>

namespace imesvc::ml {

class RmsNormImpl final : public torch::nn::Module {
public:
    RmsNormImpl(std::int64_t hidden_size, double epsilon) : epsilon_(epsilon) {
        weight = register_parameter("weight", torch::ones({hidden_size}, torch::kFloat32));
    }

    torch::Tensor forward(const torch::Tensor& input) const {
        const auto variance = input.pow(2).mean(-1, true);
        return input * torch::rsqrt(variance + epsilon_) * weight;
    }

    torch::Tensor weight;

private:
    double epsilon_;
};
TORCH_MODULE(RmsNorm);

}  // namespace imesvc::ml
