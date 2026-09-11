#pragma once

#include "core_runtime.hpp"
#include "../pipe/protocol.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ime::unix_service {

namespace detail {

std::vector<llavon::ime::core::PaddingEntry> to_core_padding(const protocol::PredictRequest& request);
std::vector<std::vector<char32_t>> to_protocol_candidates(
    const protocol::PredictRequest& request,
    const std::vector<llavon::ime::core::Prediction>& predictions);

}  // namespace detail

class ISessionEngine {
public:
    virtual ~ISessionEngine() = default;
    virtual std::vector<std::vector<char32_t>> predict(const protocol::PredictRequest& request) = 0;
    virtual bool loaded() const noexcept = 0;
};

class CoreSessionEngine final : public ISessionEngine {
public:
    explicit CoreSessionEngine(std::shared_ptr<CoreRuntime> runtime);

    std::vector<std::vector<char32_t>> predict(const protocol::PredictRequest& request) override;
    bool loaded() const noexcept override;

private:
    std::unique_ptr<llavon::ime::core::Session> session_;
};

std::unique_ptr<ISessionEngine> create_session_engine(std::shared_ptr<CoreRuntime> runtime);

}  // namespace ime::unix_service
