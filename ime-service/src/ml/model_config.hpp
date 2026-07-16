#pragma once

#include <cstdint>
#include <stdexcept>

namespace imesvc::ml {

struct LlamaModelConfig {
    static constexpr std::int64_t kVocabularySize = 18546;
    static constexpr std::int64_t kHiddenSize = 1024;
    static constexpr std::int64_t kLayers = 20;
    static constexpr std::int64_t kAttentionHeads = 16;
    static constexpr std::int64_t kKvHeads = 16;
    static constexpr std::int64_t kHeadDimension = 64;
    static constexpr std::int64_t kIntermediateSize = 2048;
    static constexpr std::int64_t kContextLength = 384;
    static constexpr std::int64_t kParameterCount = 247739392;
    static constexpr double kRmsNormEpsilon = 1e-5;
    static constexpr double kRopeTheta = 10000.0;

    std::int64_t vocabulary_size = kVocabularySize;
    std::int64_t hidden_size = kHiddenSize;
    std::int64_t layers = kLayers;
    std::int64_t attention_heads = kAttentionHeads;
    std::int64_t kv_heads = kKvHeads;
    std::int64_t intermediate_size = kIntermediateSize;
    std::int64_t context_length = kContextLength;
    double rms_norm_epsilon = kRmsNormEpsilon;
    double rope_theta = kRopeTheta;

    void validate() const {
        if (vocabulary_size != kVocabularySize || hidden_size != kHiddenSize || layers != kLayers ||
            attention_heads != kAttentionHeads || kv_heads != kKvHeads ||
            intermediate_size != kIntermediateSize || context_length != kContextLength ||
            rms_norm_epsilon != kRmsNormEpsilon || rope_theta != kRopeTheta) {
            throw std::invalid_argument("only the fixed LLaMA 250M IME architecture is supported");
        }
    }
};

struct LoraConfig {
    std::int64_t rank = 8;
    double alpha = 16.0;
    double dropout = 0.05;

    void validate() const {
        if (rank <= 0 || alpha <= 0.0 || dropout < 0.0 || dropout >= 1.0) {
            throw std::invalid_argument("invalid LoRA configuration");
        }
    }
};

}  // namespace imesvc::ml
