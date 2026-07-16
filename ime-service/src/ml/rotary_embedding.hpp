#pragma once

#include <torch/torch.h>

#include <cmath>
#include <cstdint>

namespace imesvc::ml {

class RotaryEmbedding final {
public:
    RotaryEmbedding(std::int64_t head_dimension, double theta) : head_dimension_(head_dimension), theta_(theta) {}

    std::pair<torch::Tensor, torch::Tensor> apply(const torch::Tensor& query, const torch::Tensor& key) const {
        const auto sequence = query.size(-2);
        auto positions = torch::arange(sequence, query.options().dtype(torch::kFloat32));
        auto frequencies = torch::arange(0, head_dimension_, 2, query.options().dtype(torch::kFloat32));
        frequencies = torch::pow(theta_, -frequencies / static_cast<double>(head_dimension_));
        auto angles = positions.unsqueeze(1) * frequencies.unsqueeze(0);
        auto cosine = torch::cat({angles.cos(), angles.cos()}, -1).unsqueeze(0).unsqueeze(0);
        auto sine = torch::cat({angles.sin(), angles.sin()}, -1).unsqueeze(0).unsqueeze(0);
        return {rotate(query, cosine, sine), rotate(key, cosine, sine)};
    }

private:
    static torch::Tensor rotate(const torch::Tensor& value, const torch::Tensor& cosine, const torch::Tensor& sine) {
        const auto half = value.size(-1) / 2;
        const auto rotated = torch::cat({-value.slice(-1, half), value.slice(-1, 0, half)}, -1);
        return value * cosine.to(value.dtype()) + rotated * sine.to(value.dtype());
    }

    std::int64_t head_dimension_;
    double theta_;
};

}  // namespace imesvc::ml
