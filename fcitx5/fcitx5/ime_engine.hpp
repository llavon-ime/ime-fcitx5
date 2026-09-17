#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "fcitx5/ime_config.hpp"
#include "fcitx5/input_context_property.hpp"
#include "fcitx5/prediction_coordinator.hpp"
#include "input/input_processor.hpp"
#include "input/input_session.hpp"
#include "phrase_override/phrase_override_store.hpp"

namespace fcitx {
class EventDispatcher;
}  // namespace fcitx

namespace ime::fcitx5 {

class AccessibilityContextProvider;

class ImeEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit ImeEngine(fcitx::Instance* instance);
    ~ImeEngine() override;

    void keyEvent(const fcitx::InputMethodEntry& entry, fcitx::KeyEvent& event) override;
    void activate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void deactivate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reset(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration* getConfig() const override;
    void setConfig(const fcitx::RawConfig& config) override;
    const fcitx::Configuration* getSubConfig(const std::string& path) const override;
    void setSubConfig(const std::string& path, const fcitx::RawConfig& config) override;

private:
    class StateScope {
    public:
        StateScope(ImeEngine& engine, fcitx::InputContext* input_context);
        ~StateScope();
        StateScope(const StateScope&) = delete;
        StateScope& operator=(const StateScope&) = delete;

    private:
        ImeEngine& engine_;
        bool entered_ = false;
    };

    ImeInputContextProperty* property(fcitx::InputContext* input_context) const;
    void enter_context(fcitx::InputContext* input_context);
    void leave_context();
    void reload_config();
    void apply_context_cache_limits();
    void apply_context_sources();
    void update_accessibility_status();
    void update_ui(fcitx::InputContext* input_context);

    // Applies the frontend-visible parts of an input effect. Must be called
    // with the input context entered.
    void apply_effect(fcitx::InputContext* input_context, const InputEffect& effect);
    // Enters the input context, runs an input operation on its session, and
    // applies the effect.
    void run_effect(fcitx::InputContext* input_context, const std::function<InputEffect()>& operation);

    void request_prediction_if_ready(fcitx::InputContext* input_context);
    void record_context_commit(const fcitx::InputContext* input_context, const std::u16string& text);
    void resync_context_cache(fcitx::InputContext* input_context, InputSession& session);
    fcitx::KeyList selection_key_list() const;
    fcitx::CandidateLayoutHint candidate_layout_hint() const;
    void refresh_phrase_override_editor() const;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    InputSession session_;
    FallbackEngine fallback_;
    MixedInputDecoder decoder_;
    PhraseOverrideStore phrase_overrides_;
    InputProcessor processor_;
    ServiceTransport service_transport_;
    PredictionCoordinator coordinator_;
    ImeFcitxConfig fcitx_config_;
    // Refreshed in place so a pointer handed to a config frontend stays valid.
    mutable PhraseOverrideEditorConfig phrase_override_editor_;
    Config config_;
    fcitx::Instance* instance_ = nullptr;
    fcitx::EventDispatcher* event_dispatcher_ = nullptr;
    std::unique_ptr<AccessibilityContextProvider> accessibility_context_;
    std::uint64_t accessibility_base_sequence_ = 0;
    std::uint64_t accessibility_composition_base_ = 0;
    std::size_t accessibility_max_code_units_ = 0;

    fcitx::InputContext* active_input_context_ = nullptr;
    std::size_t state_scope_depth_ = 0;
    ImeInputContextPropertyFactory property_factory_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> capability_changed_handler_;
};

class ImeEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override;
};

}  // namespace ime::fcitx5
