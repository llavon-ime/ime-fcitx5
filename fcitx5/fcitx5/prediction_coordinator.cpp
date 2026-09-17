#include "fcitx5/prediction_coordinator.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "debug/context_log.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

PredictionCoordinator::PredictionCoordinator(ServiceTransport& transport, InputProcessor& processor,
                                              Dispatch dispatch, std::weak_ptr<bool> alive,
                                              Callbacks callbacks)
    : transport_(transport),
      processor_(processor),
      dispatch_(std::move(dispatch)),
      alive_(std::move(alive)),
      callbacks_(std::move(callbacks)) {}

void PredictionCoordinator::request(fcitx::InputContext* input_context, InputSession& session) {
    callbacks_.resync_context(input_context, session);
    if (!session.prediction.begin(session.buffer.completed_segment_indices(), session.buffer.raw_composition(),
                                  session.buffer.revision())) {
        return;
    }
    const auto generation = session.prediction.generation;
    if (!session.prediction.session_open()) {
        open_session(input_context, generation);
    } else {
        send(input_context, session, generation);
    }
}

void PredictionCoordinator::close_session(InputSession& session) {
    if (!session.prediction.session_open()) return;
    transport_.close_session(session.prediction.session_id, {});
    session.prediction.session_id = {};
}

void PredictionCoordinator::open_session(fcitx::InputContext* input_context, std::uint64_t generation) {
    auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const auto alive = alive_;
    const auto dispatch = dispatch_;
    transport_.open_session([this, alive, dispatch, context, generation](protocol::Message response) mutable {
        if (alive.expired() || !dispatch) return;
        dispatch(context, [this, alive, generation, response = std::move(response)](
                              fcitx::InputContext* input_context) mutable {
                if (alive.expired()) return;
                callbacks_.run_in_session(input_context, [&](InputSession& session) {
                    if (session.prediction.generation != generation || !session.prediction.pending ||
                        session.prediction.session_open()) {
                        return;
                    }
                    if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&response)) {
                        session.prediction.session_id = opened->session_id;
                        if (auto* state = callbacks_.property(input_context)) {
                            const auto session_alive = alive_;
                            const auto session_id = session.prediction.session_id;
                            auto* transport = &transport_;
                            state->session_close_handle = [transport, session_alive, session_id]() {
                                if (session_alive.expired()) return;
                                transport->close_session(session_id, {});
                            };
                        }
                        send(input_context, session, generation);
                        return;
                    }
                    // The backend never opened a session: every requested
                    // segment keeps its fallback candidates.
                    const auto fallback_indices = session.prediction.segment_indices;
                    const bool dirty = session.prediction.finish();
                    for (const auto index : fallback_indices) {
                        processor_.apply_fallback_candidates(session, index);
                    }
                    processor_.apply_phrase_override(session);
                    if (dirty) request(input_context, session);
                    callbacks_.redraw(input_context);
                });
            });
    });
}

void PredictionCoordinator::send(fcitx::InputContext* input_context, InputSession& session,
                                 std::uint64_t generation) {
    if (session.prediction.generation != generation || !session.prediction.pending ||
        !session.prediction.session_open()) {
        return;
    }
    callbacks_.resync_context(input_context, session);
    auto request = build_request(input_context, session);
    request.request_id = session.prediction.next_request_id++;
    request.buffer_revision = session.prediction.revision;
    session.prediction.inflight_request_id = request.request_id;
    session.prediction.inflight_revision = request.buffer_revision;
    auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const auto alive = alive_;
    const auto dispatch = dispatch_;
    transport_.predict(
        request.session_id, request.request_id, request.buffer_revision, std::move(request.context),
        std::move(request.padding), [this, alive, dispatch, context, generation](protocol::Message response) mutable {
            if (alive.expired() || !dispatch) return;
            dispatch(context, [this, alive, generation, response = std::move(response)](
                                  fcitx::InputContext* input_context) mutable {
                if (alive.expired()) return;
                handle_response(input_context, generation, std::move(response));
            });
        });
}

void PredictionCoordinator::handle_response(fcitx::InputContext* input_context, std::uint64_t generation,
                                            protocol::Message response) {
    callbacks_.run_in_session(input_context, [&](InputSession& session) {
        if (session.prediction.generation != generation || !session.prediction.pending) return;

        bool accepted = false;
        if (const auto* prediction = std::get_if<protocol::Prediction>(&response)) {
            accepted = session.prediction.correlates(*prediction);
            if (accepted && session.prediction.matches_composition(
                                *prediction, session.buffer.raw_composition(), session.buffer.revision())) {
                processor_.apply_prediction(session, *prediction);
            } else if (accepted) {
                for (const auto index : session.prediction.segment_indices) {
                    processor_.apply_fallback_candidates(session, index);
                }
            }
        } else if (const auto* error = std::get_if<protocol::Error>(&response)) {
            accepted = session.prediction.correlates(*error);
            if (accepted) {
                if (error->code == protocol::ErrorCode::UnknownSession) session.prediction.session_id = {};
                for (const auto index : session.prediction.segment_indices) {
                    processor_.apply_fallback_candidates(session, index);
                }
            }
        }
        if (!accepted) return;

        processor_.apply_phrase_override(session);

        const bool dirty = session.prediction.finish();
        if (dirty) request(input_context, session);
        callbacks_.redraw(input_context);
    });
}

protocol::PredictRequest PredictionCoordinator::build_request(const fcitx::InputContext* input_context,
                                                             const InputSession& session) const {
    const auto& config = callbacks_.config();
    protocol::PredictRequest request;
    request.session_id = session.prediction.session_id;
    request.buffer_revision = session.buffer.revision();
    for (const auto& segment : session.buffer.segments()) {
        if (!segment.complete()) continue;

        protocol::PaddingEntry entry;
        entry.bopomofo = segment.reading();
        if (segment.manually_chosen && segment.selected_candidate() != 0) {
            entry.chosen = true;
            entry.chosen_char = segment.selected_candidate();
        }
        request.padding.push_back(std::move(entry));
    }

    if (input_context == nullptr ||
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return request;
    }

    const size_t reserved_tokens = 2 + request.padding.size() * 2;
    const size_t context_limit = config.context_length > static_cast<int>(reserved_tokens)
                                     ? static_cast<size_t>(config.context_length) - reserved_tokens
                                     : 0;
    request.context = session.context_cache.window(context_limit);
    log_context("model", request.context);
    return request;
}

}  // namespace ime::fcitx5
