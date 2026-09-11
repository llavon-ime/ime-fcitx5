#pragma once

#include "../engine/session_engine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ime::unix_service {

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
    mutable std::mutex mutex;
    std::condition_variable condition;
};

}  // namespace ime::unix_service
