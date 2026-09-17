#pragma once

#include <fcitx/inputcontext.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "config/config.hpp"
#include "engine/service_transport.hpp"
#include "fcitx5/input_context_property.hpp"
#include "input/input_processor.hpp"
#include "input/input_session.hpp"
#include "protocol/protocol.hpp"

namespace ime::fcitx5 {

// Adapter glue between a session's prediction bookkeeping and the Unix service
// transport: builds requests, schedules responses on the fcitx event loop, and
// applies them through the shared InputProcessor.
class PredictionCoordinator {
public:
    using ContextReference = fcitx::TrackableObjectReference<fcitx::InputContext>;
    using Dispatch = std::function<void(ContextReference, std::function<void(fcitx::InputContext*)>)>;

    struct Callbacks {
        // Returns the configuration in effect right now.
        std::function<const Config&()> config;
        // Re-reads the client's surrounding text into the session cache.
        std::function<void(fcitx::InputContext*, InputSession&)> resync_context;
        // Enters the input context and runs `body` on its active session.
        std::function<void(fcitx::InputContext*, const std::function<void(InputSession&)>&)> run_in_session;
        // Refreshes the input panel for the context.
        std::function<void(fcitx::InputContext*)> redraw;
        // Looks up the per-context property (for the session close handle).
        std::function<ImeInputContextProperty*(fcitx::InputContext*)> property;
    };

    PredictionCoordinator(ServiceTransport& transport, InputProcessor& processor, Dispatch dispatch,
                          std::weak_ptr<bool> alive, Callbacks callbacks);

    // Starts or refreshes the prediction for the session. Must be called with
    // the input context already entered.
    void request(fcitx::InputContext* input_context, InputSession& session);
    void close_session(InputSession& session);

private:
    void open_session(fcitx::InputContext* input_context, std::uint64_t generation);
    void send(fcitx::InputContext* input_context, InputSession& session, std::uint64_t generation);
    void handle_response(fcitx::InputContext* input_context, std::uint64_t generation, protocol::Message response);
    protocol::PredictRequest build_request(const fcitx::InputContext* input_context,
                                            const InputSession& session) const;

    ServiceTransport& transport_;
    InputProcessor& processor_;
    Dispatch dispatch_;
    std::weak_ptr<bool> alive_;
    Callbacks callbacks_;
};

}  // namespace ime::fcitx5
