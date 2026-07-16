#pragma once

#include "session.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <variant>

namespace imesvc {

struct SessionLimits {
    std::size_t max_sessions = 8;
    std::size_t max_idle_sessions = 4;
    std::chrono::seconds idle_timeout{1800};
    std::size_t max_concurrent_predictions = 2;
};

using OpenSessionResult = std::variant<protocol::OpenSessionResponse, protocol::Error>;
using PredictionResult = std::variant<protocol::Prediction, protocol::Error>;
using CloseSessionResult = std::variant<protocol::CloseSessionResponse, protocol::Error>;
using StatusResult = std::variant<protocol::StatusResponse, protocol::Error>;

class SessionManager final {
public:
    using Clock = std::chrono::steady_clock;
    using EngineFactory = std::function<std::unique_ptr<ISessionEngine>()>;

    SessionManager(std::shared_ptr<SharedModelRuntime> runtime, SessionLimits limits = {},
                   EngineFactory engine_factory = {});
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    const protocol::ServiceEpoch& service_epoch() const noexcept;
    const SessionLimits& limits() const noexcept;

    OpenSessionResult open_session(std::uint64_t owner_uid);
    PredictionResult predict(std::uint64_t owner_uid, const protocol::PredictRequest& request);
    [[nodiscard]] std::optional<std::string> consume_feedback_token(std::uint64_t owner_uid,
                                                                     const protocol::FeedbackRequest& request);
    CloseSessionResult close_session(std::uint64_t owner_uid, const protocol::SessionId& session_id);
    StatusResult status(std::uint64_t owner_uid, const std::optional<protocol::SessionId>& session_id = std::nullopt) const;

    void reap(Clock::time_point now = Clock::now());
    bool should_idle_shutdown(Clock::time_point now = Clock::now()) const;
    bool shutting_down() const noexcept;
    void shutdown();
    void wait_for_predictions_idle();
    void reset_engines();
    void with_predictions_stopped(const std::function<void()>& callback);
    std::size_t session_count() const;

private:
    struct SessionIdHash {
        std::size_t operator()(const protocol::SessionId& id) const noexcept;
    };

    std::shared_ptr<Session> find_session_locked(const protocol::SessionId& id) const;
    static protocol::Error error(protocol::ErrorCode code, const protocol::SessionId& id = {}, std::uint64_t request_id = 0,
                           std::uint64_t revision = 0, std::string message = {});
    bool owns(const Session& session, std::uint64_t owner_uid) const noexcept;
    void reap_locked(Clock::time_point now);
    void evict_idle_locked(std::size_t target_count);
    protocol::SessionStatus session_status(const Session& session) const;

    std::shared_ptr<SharedModelRuntime> runtime_;
    SessionLimits limits_;
    EngineFactory engine_factory_;
    bool uses_default_engine_ = false;
    protocol::ServiceEpoch service_epoch_{};

    mutable std::mutex mutex_;
    std::unordered_map<protocol::SessionId, std::shared_ptr<Session>, SessionIdHash> sessions_;
    std::chrono::steady_clock::time_point last_activity_;
    std::atomic_bool shutting_down_{false};

    mutable std::mutex prediction_mutex_;
    std::condition_variable prediction_condition_;
    std::size_t running_predictions_ = 0;
    bool model_transition_ = false;
};

}  // namespace imesvc
