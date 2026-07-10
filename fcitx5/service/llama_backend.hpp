#pragma once

#include "config/config.hpp"
#include "protocol/protocol.hpp"

#include <memory>

namespace ime::fcitx5 {

class LlamaBackend {
public:
    LlamaBackend();
    ~LlamaBackend();
    LlamaBackend(const LlamaBackend&) = delete;
    LlamaBackend& operator=(const LlamaBackend&) = delete;
    LlamaBackend(LlamaBackend&&) noexcept;
    LlamaBackend& operator=(LlamaBackend&&) noexcept;

    void load(const Config& cfg);
    bool ready() const noexcept;
    PredictResponse predict(const PredictRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ime::fcitx5
