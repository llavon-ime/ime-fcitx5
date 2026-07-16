#include "ml/llama_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace imesvc::ml {

CausalSelfAttentionImpl::CausalSelfAttentionImpl(const LlamaModelConfig& model, const LoraConfig& lora)
    : heads_(model.attention_heads), head_dimension_(model.hidden_size / model.attention_heads),
      rotary_(head_dimension_, model.rope_theta) {
    q_proj = register_module("q_proj", LoraLinear(model.hidden_size, model.hidden_size, lora));
    k_proj = register_module("k_proj", LoraLinear(model.hidden_size, model.hidden_size, lora));
    v_proj = register_module("v_proj", LoraLinear(model.hidden_size, model.hidden_size, lora));
    o_proj = register_module("o_proj", LoraLinear(model.hidden_size, model.hidden_size, lora));
}

torch::Tensor CausalSelfAttentionImpl::forward(const torch::Tensor& input) {
    const auto batch = input.size(0);
    const auto sequence = input.size(1);
    auto query = q_proj->forward(input).view({batch, sequence, heads_, head_dimension_}).transpose(1, 2);
    auto key = k_proj->forward(input).view({batch, sequence, heads_, head_dimension_}).transpose(1, 2);
    auto value = v_proj->forward(input).view({batch, sequence, heads_, head_dimension_}).transpose(1, 2);
    std::tie(query, key) = rotary_.apply(query, key);
    auto scores = torch::matmul(query, key.transpose(-2, -1)) / std::sqrt(static_cast<double>(head_dimension_));
    const auto causal = torch::ones({sequence, sequence}, input.options().dtype(torch::kBool)).triu(1);
    scores = scores.masked_fill(causal.unsqueeze(0).unsqueeze(0), -std::numeric_limits<float>::infinity());
    auto values = torch::matmul(torch::softmax(scores, -1), value).transpose(1, 2).contiguous();
    return o_proj->forward(values.view({batch, sequence, heads_ * head_dimension_}));
}

std::vector<torch::Tensor> CausalSelfAttentionImpl::adapter_parameters() const {
    auto parameters = q_proj->adapter_parameters();
    for (const auto& module : {k_proj, v_proj, o_proj}) {
        const auto values = module->adapter_parameters();
        parameters.insert(parameters.end(), values.begin(), values.end());
    }
    return parameters;
}

LlamaMlpImpl::LlamaMlpImpl(const LlamaModelConfig& model, const LoraConfig& lora) {
    gate_proj = register_module("gate_proj", LoraLinear(model.hidden_size, model.intermediate_size, lora));
    up_proj = register_module("up_proj", LoraLinear(model.hidden_size, model.intermediate_size, lora));
    down_proj = register_module("down_proj", LoraLinear(model.intermediate_size, model.hidden_size, lora));
}

torch::Tensor LlamaMlpImpl::forward(const torch::Tensor& input) {
    return down_proj->forward(torch::silu(gate_proj->forward(input)) * up_proj->forward(input));
}

std::vector<torch::Tensor> LlamaMlpImpl::adapter_parameters() const {
    auto parameters = gate_proj->adapter_parameters();
    for (const auto& module : {up_proj, down_proj}) {
        const auto values = module->adapter_parameters();
        parameters.insert(parameters.end(), values.begin(), values.end());
    }
    return parameters;
}

LlamaDecoderLayerImpl::LlamaDecoderLayerImpl(const LlamaModelConfig& model, const LoraConfig& lora) {
    self_attn = register_module("self_attn", CausalSelfAttention(model, lora));
    mlp = register_module("mlp", LlamaMlp(model, lora));
    input_layernorm = register_module("input_layernorm", RmsNorm(model.hidden_size, model.rms_norm_epsilon));
    post_attention_layernorm = register_module("post_attention_layernorm", RmsNorm(model.hidden_size, model.rms_norm_epsilon));
}

torch::Tensor LlamaDecoderLayerImpl::forward(const torch::Tensor& input) {
    auto hidden = input + self_attn->forward(input_layernorm->forward(input));
    return hidden + mlp->forward(post_attention_layernorm->forward(hidden));
}

std::vector<torch::Tensor> LlamaDecoderLayerImpl::adapter_parameters() const {
    auto parameters = self_attn->adapter_parameters();
    const auto mlp_parameters = mlp->adapter_parameters();
    parameters.insert(parameters.end(), mlp_parameters.begin(), mlp_parameters.end());
    return parameters;
}

LlamaForCausalLmImpl::LlamaForCausalLmImpl(LlamaModelConfig model, LoraConfig lora) : model_(model) {
    model_.validate();
    lora.validate();
    embed_tokens = register_module("embed_tokens", torch::nn::Embedding(model_.vocabulary_size, model_.hidden_size));
    layers = register_module("layers", torch::nn::ModuleList());
    for (std::int64_t index = 0; index < model_.layers; ++index) layers->push_back(LlamaDecoderLayer(model_, lora));
    norm = register_module("norm", RmsNorm(model_.hidden_size, model_.rms_norm_epsilon));
    lm_head = register_module("lm_head", torch::nn::Linear(torch::nn::LinearOptions(model_.hidden_size, model_.vocabulary_size).bias(false)));
    freeze_base_parameters();
}

torch::Tensor LlamaForCausalLmImpl::forward(const torch::Tensor& token_ids) {
    if (token_ids.dim() != 2 || token_ids.size(1) > model_.context_length) throw std::invalid_argument("invalid LLaMA token shape");
    auto hidden = embed_tokens->forward(token_ids);
    for (const auto& layer : *layers) {
        hidden = std::dynamic_pointer_cast<LlamaDecoderLayerImpl>(layer)->forward(hidden);
    }
    return lm_head->forward(norm->forward(hidden));
}

std::vector<torch::Tensor> LlamaForCausalLmImpl::adapter_parameters() const {
    std::vector<torch::Tensor> result;
    for (const auto& layer : *layers) {
        const auto values = std::dynamic_pointer_cast<LlamaDecoderLayerImpl>(layer)->adapter_parameters();
        result.insert(result.end(), values.begin(), values.end());
    }
    return result;
}

std::unordered_map<std::string, torch::Tensor> LlamaForCausalLmImpl::safetensors_parameters() const {
    std::unordered_map<std::string, torch::Tensor> result{{"model.embed_tokens.weight", embed_tokens->weight},
                                                           {"model.norm.weight", norm->weight},
                                                           {"lm_head.weight", lm_head->weight}};
    for (std::int64_t index = 0; index < model_.layers; ++index) {
        const auto layer = std::dynamic_pointer_cast<LlamaDecoderLayerImpl>((*layers)[index]);
        const auto prefix = "model.layers." + std::to_string(index) + ".";
        result.emplace(prefix + "self_attn.q_proj.weight", layer->self_attn->q_proj->weight);
        result.emplace(prefix + "self_attn.k_proj.weight", layer->self_attn->k_proj->weight);
        result.emplace(prefix + "self_attn.v_proj.weight", layer->self_attn->v_proj->weight);
        result.emplace(prefix + "self_attn.o_proj.weight", layer->self_attn->o_proj->weight);
        result.emplace(prefix + "mlp.gate_proj.weight", layer->mlp->gate_proj->weight);
        result.emplace(prefix + "mlp.up_proj.weight", layer->mlp->up_proj->weight);
        result.emplace(prefix + "mlp.down_proj.weight", layer->mlp->down_proj->weight);
        result.emplace(prefix + "input_layernorm.weight", layer->input_layernorm->weight);
        result.emplace(prefix + "post_attention_layernorm.weight", layer->post_attention_layernorm->weight);
    }
    return result;
}

std::unordered_map<std::string, torch::Tensor> LlamaForCausalLmImpl::adapter_tensors() const {
    std::unordered_map<std::string, torch::Tensor> result;
    for (std::int64_t index = 0; index < model_.layers; ++index) {
        const auto layer = std::dynamic_pointer_cast<LlamaDecoderLayerImpl>((*layers)[index]);
        const auto prefix = "blk." + std::to_string(index) + ".";
        const auto add = [&result, &prefix](const char* name, const LoraLinear& linear) {
            result.emplace(prefix + name + ".weight.lora_a", linear->lora_a);
            result.emplace(prefix + name + ".weight.lora_b", linear->lora_b);
        };
        add("attn_q", layer->self_attn->q_proj); add("attn_k", layer->self_attn->k_proj);
        add("attn_v", layer->self_attn->v_proj); add("attn_output", layer->self_attn->o_proj);
        add("ffn_gate", layer->mlp->gate_proj); add("ffn_up", layer->mlp->up_proj); add("ffn_down", layer->mlp->down_proj);
    }
    return result;
}

std::unordered_map<std::string, torch::Tensor> LlamaForCausalLmImpl::gguf_adapter_tensors() const {
    auto result = adapter_tensors();
    const auto permute_output_rows = [this](const torch::Tensor& tensor) {
        const auto rows_per_half_head = tensor.size(0) / model_.attention_heads / 2;
        return tensor.view({model_.attention_heads, 2, rows_per_half_head, tensor.size(1)})
            .transpose(1, 2).contiguous().view_as(tensor);
    };
    for (std::int64_t index = 0; index < model_.layers; ++index) {
        const auto prefix = "blk." + std::to_string(index) + ".";
        for (const auto* projection : {"attn_q", "attn_k"}) {
            auto& tensor = result.at(prefix + projection + ".weight.lora_b");
            tensor = permute_output_rows(tensor);
        }
    }
    return result;
}

void LlamaForCausalLmImpl::freeze_base_parameters() {
    for (const auto& parameter : parameters()) parameter.set_requires_grad(false);
    for (const auto& parameter : adapter_parameters()) parameter.set_requires_grad(true);
}

void LlamaForCausalLmImpl::assert_gradient_isolation() const {
    const auto adapters = adapter_parameters();
    for (const auto& named : named_parameters()) {
        const bool adapter = std::any_of(adapters.begin(), adapters.end(), [&named](const auto& value) { return value.is_same(named.value()); });
        if (!adapter && (named.value().requires_grad() || named.value().grad().defined())) {
            throw std::logic_error("a frozen LLaMA base parameter received a gradient: " + named.key());
        }
    }
}

}  // namespace imesvc::ml
