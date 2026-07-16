#pragma once

#include "ml/lora_linear.hpp"
#include "ml/model_config.hpp"
#include "ml/rms_norm.hpp"
#include "ml/rotary_embedding.hpp"

#include <torch/torch.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace imesvc::ml {

class CausalSelfAttentionImpl final : public torch::nn::Module {
public:
    CausalSelfAttentionImpl(const LlamaModelConfig& model, const LoraConfig& lora);
    torch::Tensor forward(const torch::Tensor& input);
    std::vector<torch::Tensor> adapter_parameters() const;

    LoraLinear q_proj{nullptr};
    LoraLinear k_proj{nullptr};
    LoraLinear v_proj{nullptr};
    LoraLinear o_proj{nullptr};

private:
    std::int64_t heads_;
    std::int64_t head_dimension_;
    RotaryEmbedding rotary_;
};
TORCH_MODULE(CausalSelfAttention);

class LlamaMlpImpl final : public torch::nn::Module {
public:
    LlamaMlpImpl(const LlamaModelConfig& model, const LoraConfig& lora);
    torch::Tensor forward(const torch::Tensor& input);
    std::vector<torch::Tensor> adapter_parameters() const;

    LoraLinear gate_proj{nullptr};
    LoraLinear up_proj{nullptr};
    LoraLinear down_proj{nullptr};
};
TORCH_MODULE(LlamaMlp);

class LlamaDecoderLayerImpl final : public torch::nn::Module {
public:
    LlamaDecoderLayerImpl(const LlamaModelConfig& model, const LoraConfig& lora);
    torch::Tensor forward(const torch::Tensor& input);
    std::vector<torch::Tensor> adapter_parameters() const;

    CausalSelfAttention self_attn{nullptr};
    LlamaMlp mlp{nullptr};
    RmsNorm input_layernorm{nullptr};
    RmsNorm post_attention_layernorm{nullptr};
};
TORCH_MODULE(LlamaDecoderLayer);

class LlamaForCausalLmImpl final : public torch::nn::Module {
public:
    LlamaForCausalLmImpl(LlamaModelConfig model = {}, LoraConfig lora = {});

    torch::Tensor forward(const torch::Tensor& token_ids);
    [[nodiscard]] std::vector<torch::Tensor> adapter_parameters() const;
    [[nodiscard]] std::unordered_map<std::string, torch::Tensor> safetensors_parameters() const;
    [[nodiscard]] std::unordered_map<std::string, torch::Tensor> adapter_tensors() const;
    [[nodiscard]] std::unordered_map<std::string, torch::Tensor> gguf_adapter_tensors() const;
    void freeze_base_parameters();
    void assert_gradient_isolation() const;

    torch::nn::Embedding embed_tokens{nullptr};
    torch::nn::ModuleList layers{nullptr};
    RmsNorm norm{nullptr};
    torch::nn::Linear lm_head{nullptr};

private:
    LlamaModelConfig model_;
};
TORCH_MODULE(LlamaForCausalLm);

}  // namespace imesvc::ml
