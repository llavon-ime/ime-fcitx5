#pragma once

#include "config/config.hpp"
#include "protocol/protocol.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace ime::fcitx5 {

enum class PredictState { Pending, Ready, Unavailable };

class ServiceClient {
public:
    explicit ServiceClient(std::filesystem::path socket = socket_path());
    ~ServiceClient();

    ServiceClient(const ServiceClient&) = delete;
    ServiceClient& operator=(const ServiceClient&) = delete;

    PredictState request_predict_async(PredictRequest request);
    PredictState request_predict_async(PredictRequest request, std::function<void(PredictState)> on_complete);
    std::optional<PredictResponse> latest_response();
    PredictState state();
    StatusResponse status();
    StatusResponse stop();
    bool start_service_if_needed();

private:
    PredictResponse request_predict(const PredictRequest& request);
    void finish_worker();

    std::filesystem::path socket_path_;
    std::mutex mutex_;
    std::thread worker_;
    PredictState state_ = PredictState::Unavailable;
    std::optional<PredictResponse> latest_response_;
};

}  // namespace ime::fcitx5
