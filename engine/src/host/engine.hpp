#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "config/config.hpp"
#include "context/accessibility_context.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "host/host.hpp"
#include "host/render_state.hpp"
#include "input/input_processor.hpp"
#include "input/input_session.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include "protocol/protocol.hpp"

namespace llavon::ime {

struct EngineOptions {
    std::filesystem::path table_path;
    std::filesystem::path phrase_overrides_path;
    ServiceTransportOptions transport;
    Config config;
    // When false (macOS InputMethodKit, headless hosts) the engine never
    // creates the AT-SPI context provider and relies on Host::surrounding_text
    // alone.
    bool enable_accessibility = true;
};

// Host-agnostic input method engine: owns the per-context sessions, routes
// keys, drives the prediction service, and produces render states. Hosts
// implement the Host callbacks and draw whatever render_state() returns.
class Engine {
public:
    Engine(EngineOptions options, Host& host);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Context lifecycle. attach is idempotent; detach closes the service
    // session and drops all state for the context.
    void attach(ContextId context);
    void detach(ContextId context);
    bool has_context(ContextId context) const;

    // Routes one key, including releases: the engine decides whether the key
    // is consumed. Returns true when the host must consume the event.
    bool key_event(ContextId context, const InputKey& key);
    void select_candidate(ContextId context, int index);
    void select_symbol(ContextId context, int index, std::uint64_t epoch);

    // Input method lifecycle events.
    void activate(ContextId context);
    void deactivate(ContextId context);
    void reset(ContextId context, InputResetReason reason, bool clear_context);

    // Current panel state; hosts call this after update_ui().
    RenderState render_state(ContextId context);

    // Settings. set_config applies to every context; when `settle_sessions`
    // is true it also settles pending input and closes prediction sessions so
    // stale results cannot leak across the change (the config UI path). A
    // plain reload from disk only refreshes the config and context sources.
    void set_config(Config config, bool settle_sessions = true);
    void reload_phrase_overrides();
    const Config& config() const { return config_; }
    PhraseOverrideStore& phrase_overrides() { return phrase_overrides_; }

    // Drops any cached context text (used when a context becomes sensitive).
    void clear_context_text(ContextId context);
    AccessibilityContextState accessibility_state() const;

    // Raw session access for host diagnostics and tests. The engine keeps
    // ownership; prefer the event API for normal operation.
    InputSession* session(ContextId context) { return find(context); }

private:
    InputSession* find(ContextId context);
    InputSession& find_or_create(ContextId context);
    void apply_effect(ContextId context, InputSession& session, const InputEffect& effect);

    void request_prediction(ContextId context, InputSession& session);
    void open_prediction_session(ContextId context, std::uint64_t generation);
    void send_prediction(ContextId context, InputSession& session, std::uint64_t generation);
    void handle_prediction_response(ContextId context, std::uint64_t generation, protocol::Message response);
    void close_prediction_session(InputSession& session);
    void resync_context(ContextId context, InputSession& session);
    protocol::PredictRequest build_predict_request(ContextId context, const InputSession& session) const;
    std::optional<std::u16string> strip_accessibility_preedit(const InputSession& session,
                                                              const std::u16string& sample) const;
    void apply_context_sources();

    EngineOptions options_;
    Host& host_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    FallbackEngine fallback_;
    MixedInputDecoder decoder_;
    PhraseOverrideStore phrase_overrides_;
    InputProcessor processor_;
    ServiceTransport transport_;
    Config config_;
    std::unordered_map<ContextId, std::unique_ptr<InputSession>> sessions_;
    std::unique_ptr<AccessibilityContextProvider> accessibility_context_;
    std::uint64_t accessibility_base_sequence_ = 0;
    std::uint64_t accessibility_composition_base_ = 0;
    std::size_t accessibility_max_code_units_ = 0;
};

}  // namespace llavon::ime
