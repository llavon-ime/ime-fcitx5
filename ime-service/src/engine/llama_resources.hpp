#pragma once

#include "engine/model_runtime.hpp"
#include "engine/tokenizer.hpp"

#include <llama-cpp.h>

#include <cstdint>
#include <memory>

namespace imesvc {

class LlamaModelResources final {
public:
    explicit LlamaModelResources(RuntimeConfig config);

    [[nodiscard]] llama_model* model() const noexcept;
    [[nodiscard]] const llama_vocab* vocab() const noexcept;
    [[nodiscard]] llama_context* new_context(std::uint32_t context_length = 0,
                                             std::uint32_t batch_size = 0) const;
    [[nodiscard]] const Tokenizer& tokenizer() const noexcept;

private:
    RuntimeConfig config_;
    Tokenizer tokenizer_;
    llama_model_ptr model_;
    const llama_vocab* vocab_ = nullptr;
};

#ifdef _WIN32
[[nodiscard]] std::shared_ptr<LlamaModelResources> legacy_llama_resources();
#endif

}  // namespace imesvc
