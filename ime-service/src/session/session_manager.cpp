#include "session_manager.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace imesvc {

namespace {

template <typename Array>
void random_bytes(Array& bytes) {
    std::random_device device;
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(device());
    if (std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; })) bytes[0] = 1;
}

std::vector<char32_t> utf16_scalars(const std::u16string& value) {
    std::vector<char32_t> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto scalar = static_cast<std::uint32_t>(value[index]);
        if (scalar >= 0xd800U && scalar <= 0xdbffU && index + 1 < value.size()) {
            const auto low = static_cast<std::uint32_t>(value[++index]);
            scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
        }
        result.push_back(static_cast<char32_t>(scalar));
    }
    return result;
}

}  // namespace

SessionManager::SessionManager(std::shared_ptr<SharedModelRuntime> runtime, SessionLimits limits,
                               EngineFactory engine_factory)
    : runtime_(std::move(runtime)), limits_(limits), engine_factory_(std::move(engine_factory)),
      last_activity_(Clock::now()) {
    if (!runtime_) throw std::invalid_argument("session manager requires a runtime");
    if (limits_.max_sessions == 0 || limits_.max_concurrent_predictions == 0) {
        throw std::invalid_argument("session limits must be positive");
    }
    if (limits_.max_idle_sessions == 0) limits_.max_idle_sessions = limits_.max_sessions;
    if (limits_.max_idle_sessions > limits_.max_sessions) limits_.max_idle_sessions = limits_.max_sessions;
    random_bytes(service_epoch_);
    if (!engine_factory_) {
        uses_default_engine_ = true;
        engine_factory_ = [runtime = runtime_]() { return create_session_engine(runtime); };
    }
}

SessionManager::~SessionManager() {
    shutdown();
}

const protocol::ServiceEpoch& SessionManager::service_epoch() const noexcept {
    return service_epoch_;
}

const SessionLimits& SessionManager::limits() const noexcept {
    return limits_;
}

std::size_t SessionManager::SessionIdHash::operator()(const protocol::SessionId& id) const noexcept {
    std::size_t hash = 1469598103934665603ULL;
    for (const auto byte : id) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

OpenSessionResult SessionManager::open_session(std::uint64_t owner_uid) {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");

    const auto now = Clock::now();
    reap_locked(now);
    evict_idle_locked(limits_.max_idle_sessions > 0 ? limits_.max_idle_sessions - 1 : 0);
    if (sessions_.size() >= limits_.max_sessions) {
        return error(protocol::ErrorCode::ResourceExhausted, {}, 0, 0, "maximum session count reached");
    }

    auto session = std::make_shared<Session>();
    do {
        random_bytes(session->id);
    } while (sessions_.contains(session->id));
    session->owner_uid = owner_uid;
    session->created_at = now;
    session->last_used = now;
    sessions_.emplace(session->id, session);
    last_activity_ = now;
    return protocol::OpenSessionResponse{session->id, service_epoch_};
}

PredictionResult SessionManager::predict(std::uint64_t owner_uid, const protocol::PredictRequest& request) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            return error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                         request.buffer_revision, "service is shutting down");
        }
        session = find_session_locked(request.session_id);
        if (!session) {
            return error(protocol::ErrorCode::UnknownSession, request.session_id, request.request_id,
                         request.buffer_revision, "unknown session");
        }
        if (!owns(*session, owner_uid)) {
            return error(protocol::ErrorCode::Unauthorized, request.session_id, request.request_id,
                         request.buffer_revision, "session belongs to another user");
        }
    }

    {
        std::lock_guard lock(session->mutex);
        if (session->state == SessionState::Closing) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "session is closing");
        }
        if (session->prediction_in_flight && request.request_id == session->last_request_id) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "duplicate prediction is already in flight");
        }
        if (request.request_id == 0 || request.request_id <= session->last_request_id) {
            return error(protocol::ErrorCode::OutOfOrder, request.session_id, request.request_id,
                         request.buffer_revision, "request id is not strictly increasing");
        }
        if (session->prediction_in_flight) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "session already has a prediction in flight");
        }
        session->prediction_in_flight = true;
        session->last_request_id = request.request_id;
        session->last_used = Clock::now();
    }

    {
        std::unique_lock lock(prediction_mutex_);
        prediction_condition_.wait(lock, [this, &session]() {
            std::lock_guard session_lock(session->mutex);
            return shutting_down_ || session->state == SessionState::Closing ||
                   (!model_transition_ && running_predictions_ < limits_.max_concurrent_predictions);
        });
        {
            std::lock_guard session_lock(session->mutex);
            if (session->state == SessionState::Closing) {
                lock.unlock();
                {
                    std::lock_guard cancelled_lock(session->mutex);
                    session->prediction_in_flight = false;
                }
                session->condition.notify_all();
                return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                             request.buffer_revision, "queued prediction was cancelled by close");
            }
        }
        if (shutting_down_) {
            lock.unlock();
            std::lock_guard session_lock(session->mutex);
            session->prediction_in_flight = false;
            session->condition.notify_all();
            return error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                         request.buffer_revision, "service is shutting down");
        }
        ++running_predictions_;
    }

    auto finish = [this, &session]() {
        {
            std::lock_guard lock(prediction_mutex_);
            if (running_predictions_ > 0) --running_predictions_;
        }
        prediction_condition_.notify_one();
        {
            std::lock_guard lock(session->mutex);
            session->prediction_in_flight = false;
            session->last_used = Clock::now();
        }
        session->condition.notify_all();
        std::lock_guard lock(mutex_);
        last_activity_ = Clock::now();
    };

    try {
        std::vector<std::vector<char32_t>> candidates;
        {
            std::lock_guard lock(session->mutex);
            if (uses_default_engine_) runtime_->ensure_loaded();
            if (!session->engine) session->engine = engine_factory_();
            if (!session->engine) throw std::runtime_error("session engine allocation failed");
            candidates = session->engine->predict(request);
        }
        if (candidates.size() != request.padding.size()) {
            finish();
            return error(protocol::ErrorCode::ProtocolError, request.session_id, request.request_id,
                         request.buffer_revision, "prediction segment count does not match padding");
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (request.padding[i].chosen &&
                (candidates[i].size() != 1 || candidates[i].front() != request.padding[i].chosen_char)) {
                finish();
                return error(protocol::ErrorCode::ProtocolError, request.session_id, request.request_id,
                             request.buffer_revision, "chosen prediction is not a singleton");
            }
        }
        protocol::Prediction response{request.session_id, request.request_id, request.buffer_revision, std::move(candidates)};
        random_bytes(response.feedback_token);
        {
            std::lock_guard lock(session->mutex);
            session->feedback_token = response.feedback_token;
            session->feedback_request_id = request.request_id;
            session->feedback_adapter_version = session->engine->adapter_version();
            session->feedback_context = request.context;
            auto authenticated_candidates = response.candidates;
            for (std::size_t index = 0; index < request.padding.size(); ++index) {
                if (!request.padding[index].chosen || index >= session->feedback_padding.size() ||
                    index >= session->feedback_candidates.size() ||
                    request.padding[index].bopomofo != session->feedback_padding[index].bopomofo) {
                    continue;
                }
                const auto& previous = session->feedback_candidates[index];
                if (std::find(previous.begin(), previous.end(), request.padding[index].chosen_char) != previous.end()) {
                    authenticated_candidates[index] = previous;
                }
            }
            session->feedback_padding = request.padding;
            session->feedback_candidates = std::move(authenticated_candidates);
            session->feedback_created_at = Clock::now();
        }
        finish();
        return response;
    } catch (const std::bad_alloc&) {
        finish();
        return error(protocol::ErrorCode::ResourceExhausted, request.session_id, request.request_id,
                     request.buffer_revision, "prediction allocation failed");
    } catch (const std::exception& exception) {
        finish();
        return error(protocol::ErrorCode::ModelError, request.session_id, request.request_id,
                     request.buffer_revision, exception.what());
    }
}

std::optional<std::string> SessionManager::consume_feedback_token(std::uint64_t owner_uid,
                                                                  const protocol::FeedbackRequest& request) {
    std::lock_guard lock(mutex_);
    const auto session = find_session_locked(request.session_id);
    if (!session || !owns(*session, owner_uid)) return std::nullopt;
    std::lock_guard session_lock(session->mutex);
    if (request.feedback_token != session->feedback_token || protocol::is_zero(session->feedback_token) ||
        request.left_context != session->feedback_context || Clock::now() - session->feedback_created_at > std::chrono::minutes(10)) {
        return std::nullopt;
    }
    const auto committed = utf16_scalars(request.committed_characters);
    const auto predicted = utf16_scalars(request.predicted_top1);
    std::vector<std::u16string> readings;
    std::size_t reading_begin = 0;
    while (reading_begin < request.bopomofo_sequence.size()) {
        const auto reading_end = request.bopomofo_sequence.find(protocol::kBopomofoReadingSeparator, reading_begin);
        readings.push_back(request.bopomofo_sequence.substr(
            reading_begin, reading_end == std::u16string::npos ? reading_end : reading_end - reading_begin));
        if (reading_end == std::u16string::npos) break;
        reading_begin = reading_end + 1;
    }
    if (committed.size() != session->feedback_candidates.size() || predicted.size() != committed.size() ||
        readings.size() != committed.size() || session->feedback_padding.size() != committed.size() ||
        request.manually_chosen_flags.size() != committed.size()) {
        return std::nullopt;
    }
    bool saw_manual = false;
    bool saw_difference = false;
    for (std::size_t index = 0; index < committed.size(); ++index) {
        const auto& candidates = session->feedback_candidates[index];
        const bool manual = request.manually_chosen_flags[index];
        const bool differs = committed[index] != predicted[index];
        if (readings[index] != session->feedback_padding[index].bopomofo || candidates.empty() ||
            candidates.front() != predicted[index] || (differs && !manual) ||
            std::find(candidates.begin(), candidates.end(), committed[index]) == candidates.end()) {
            return std::nullopt;
        }
        saw_manual = saw_manual || manual;
        saw_difference = saw_difference || differs;
    }
    switch (request.signal_type) {
        case protocol::FeedbackSignal::ExplicitCorrection:
            if (!saw_manual || !saw_difference) return std::nullopt;
            break;
        case protocol::FeedbackSignal::ExplicitTop1Selection:
            if (!saw_manual || saw_difference) return std::nullopt;
            break;
        case protocol::FeedbackSignal::AcceptedPrediction:
            if (saw_manual || saw_difference) return std::nullopt;
            break;
        case protocol::FeedbackSignal::FallbackCommit:
            return std::nullopt;
    }
    auto adapter_version = session->feedback_adapter_version;
    session->feedback_token = {};
    session->feedback_request_id = 0;
    session->feedback_adapter_version.clear();
    session->feedback_context.clear();
    session->feedback_padding.clear();
    session->feedback_candidates.clear();
    return adapter_version;
}

CloseSessionResult SessionManager::close_session(std::uint64_t owner_uid, const protocol::SessionId& session_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        session = find_session_locked(session_id);
        if (!session) return error(protocol::ErrorCode::UnknownSession, session_id, 0, 0, "unknown session");
        if (!owns(*session, owner_uid)) return error(protocol::ErrorCode::Unauthorized, session_id, 0, 0, "session belongs to another user");
        {
            std::lock_guard session_lock(session->mutex);
            session->state = SessionState::Closing;
        }
        prediction_condition_.notify_all();
    }

    std::unique_lock session_lock(session->mutex);
    session->condition.wait(session_lock, [&session]() { return !session->prediction_in_flight; });
    session->engine.reset();
    session_lock.unlock();

    {
        std::lock_guard lock(mutex_);
        sessions_.erase(session_id);
        last_activity_ = Clock::now();
    }
    return protocol::CloseSessionResponse{session_id, true};
}

StatusResult SessionManager::status(std::uint64_t owner_uid, const std::optional<protocol::SessionId>& session_id) const {
    StatusResult result;
    protocol::StatusResponse response;
    response.service_epoch = service_epoch_;
    {
        std::lock_guard lock(mutex_);
        response.shutting_down = shutting_down_;
        response.active_sessions = static_cast<std::uint32_t>(std::min<std::size_t>(sessions_.size(), std::numeric_limits<std::uint32_t>::max()));
        response.max_sessions = static_cast<std::uint32_t>(std::min<std::size_t>(limits_.max_sessions, std::numeric_limits<std::uint32_t>::max()));
        const auto runtime_status = runtime_->status();
        response.model_loaded = runtime_status.inference_model_loaded;
        response.tables_loaded = runtime_status.tables_loaded;
        response.inference_model_loaded = runtime_status.inference_model_loaded;
        response.adapter_loaded = runtime_status.adapter_loaded;
        response.active_adapter_version = runtime_status.active_adapter_version;
        response.model_error = runtime_status.error;
        if (session_id) {
            const auto session = find_session_locked(*session_id);
            if (!session) return error(protocol::ErrorCode::UnknownSession, *session_id, 0, 0, "unknown session");
            if (!owns(*session, owner_uid)) return error(protocol::ErrorCode::Unauthorized, *session_id, 0, 0, "session belongs to another user");
            response.session = session_status(*session);
        }
    }
    return response;
}

void SessionManager::reap(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    reap_locked(now);
    evict_idle_locked(limits_.max_idle_sessions);
}

bool SessionManager::should_idle_shutdown(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || !sessions_.empty()) return false;
    std::lock_guard prediction_lock(prediction_mutex_);
    return running_predictions_ == 0 && now - last_activity_ >= limits_.idle_timeout;
}

bool SessionManager::shutting_down() const noexcept {
    std::lock_guard lock(mutex_);
    return shutting_down_;
}

void SessionManager::shutdown() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_ && sessions_.empty()) return;
        shutting_down_ = true;
        for (auto& [id, session] : sessions_) {
            std::lock_guard session_lock(session->mutex);
            session->state = SessionState::Closing;
            sessions.push_back(session);
        }
    }
    {
        std::lock_guard lock(prediction_mutex_);
        prediction_condition_.notify_all();
    }
    for (const auto& session : sessions) {
        std::unique_lock lock(session->mutex);
        session->condition.wait(lock, [&session]() { return !session->prediction_in_flight; });
        session->engine.reset();
    }
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

void SessionManager::wait_for_predictions_idle() {
    std::unique_lock lock(prediction_mutex_);
    prediction_condition_.wait(lock, [this]() { return running_predictions_ == 0; });
}

void SessionManager::reset_engines() {
    wait_for_predictions_idle();
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(mutex_);
        sessions.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_) sessions.push_back(session);
    }
    for (const auto& session : sessions) {
        std::lock_guard lock(session->mutex);
        if (session->state == SessionState::Closing) continue;
        session->engine.reset();
        session->feedback_token = {};
        session->feedback_request_id = 0;
        session->feedback_adapter_version.clear();
        session->feedback_context.clear();
        session->feedback_padding.clear();
        session->feedback_candidates.clear();
    }
}

void SessionManager::with_predictions_stopped(const std::function<void()>& callback) {
    {
        std::unique_lock lock(prediction_mutex_);
        model_transition_ = true;
        prediction_condition_.wait(lock, [this]() { return running_predictions_ == 0; });
    }
    try {
        callback();
    } catch (...) {
        std::lock_guard lock(prediction_mutex_);
        model_transition_ = false;
        prediction_condition_.notify_all();
        throw;
    }
    {
        std::lock_guard lock(prediction_mutex_);
        model_transition_ = false;
    }
    prediction_condition_.notify_all();
}

std::size_t SessionManager::session_count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::shared_ptr<Session> SessionManager::find_session_locked(const protocol::SessionId& id) const {
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

protocol::Error SessionManager::error(protocol::ErrorCode code, const protocol::SessionId& id, std::uint64_t request_id,
                                std::uint64_t revision, std::string message) {
    return protocol::Error{code, id, request_id, revision, std::move(message)};
}

bool SessionManager::owns(const Session& session, std::uint64_t owner_uid) const noexcept {
    return session.owner_uid == owner_uid;
}

void SessionManager::reap_locked(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto& session = it->second;
        bool expired = false;
        {
            std::lock_guard lock(session->mutex);
            expired = session->state == SessionState::Active && !session->prediction_in_flight &&
                      now - session->last_used >= limits_.idle_timeout;
        }
        if (expired) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SessionManager::evict_idle_locked(std::size_t target_count) {
    auto idle_count = [&]() {
        std::size_t count = 0;
        for (const auto& [id, session] : sessions_) {
            std::lock_guard lock(session->mutex);
            if (session->state == SessionState::Active && !session->prediction_in_flight) ++count;
        }
        return count;
    };

    while (idle_count() > target_count) {
        auto oldest = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            const auto& session = it->second;
            std::lock_guard lock(session->mutex);
            if (session->state != SessionState::Active || session->prediction_in_flight) continue;
            if (oldest == sessions_.end() || session->last_used < oldest->second->last_used) oldest = it;
        }
        if (oldest == sessions_.end()) break;
        sessions_.erase(oldest);
    }
}

protocol::SessionStatus SessionManager::session_status(const Session& session) const {
    std::lock_guard lock(session.mutex);
    return protocol::SessionStatus{session.id,
                             session.state == SessionState::Active,
                             session.state == SessionState::Closing,
                             session.engine != nullptr && session.engine->loaded(),
                             session.last_request_id};
}

}  // namespace imesvc
