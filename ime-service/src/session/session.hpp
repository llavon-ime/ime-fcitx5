#pragma once

#include "../engine/session_engine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace imesvc {

enum class SessionState : std::uint8_t {
    Active,
    Closing,
};

struct Session {
    protocol::SessionId id{};
    std::uint64_t owner_uid = 0;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_used;
    SessionState state = SessionState::Active;
    std::unique_ptr<ISessionEngine> engine;
    std::uint64_t last_request_id = 0;
    bool prediction_in_flight = false;
    protocol::EventId feedback_token{};
    std::uint64_t feedback_request_id = 0;
    std::string feedback_adapter_version;
    std::u16string feedback_context;
    std::vector<protocol::PaddingEntry> feedback_padding;
    std::vector<std::vector<char32_t>> feedback_candidates;
    std::chrono::steady_clock::time_point feedback_created_at;
    mutable std::mutex mutex;
    std::condition_variable condition;
};

}  // namespace imesvc
