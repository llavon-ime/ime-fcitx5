#pragma once

#include "model_runtime.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace imesvc {

class ISessionEngine {
public:
    virtual ~ISessionEngine() = default;
    virtual std::vector<std::vector<char32_t>> predict(const protocol::PredictRequest& request) = 0;
    virtual bool loaded() const noexcept = 0;
    virtual std::string adapter_version() const { return {}; }
};

class SessionEngine final : public ISessionEngine {
public:
    explicit SessionEngine(std::shared_ptr<SharedModelRuntime> runtime);

    std::vector<std::vector<char32_t>> predict(const protocol::PredictRequest& request) override;
    bool loaded() const noexcept override;

    std::uint64_t next_position() const noexcept;
    const std::vector<char32_t>& previous_tokens() const noexcept;

private:
    std::shared_ptr<SharedModelRuntime> runtime_;
    std::vector<char32_t> previous_tokens_;
    std::uint64_t next_position_ = 0;
};

std::unique_ptr<ISessionEngine> create_session_engine(std::shared_ptr<SharedModelRuntime> runtime);

}  // namespace imesvc
