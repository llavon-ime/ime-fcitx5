#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>

#include <memory>
#include <optional>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "fcitx5/ime_config.hpp"
#include "fcitx5/input_context_property.hpp"

namespace fcitx {
class EventDispatcher;
class Instance;
}  // namespace fcitx

namespace ime::fcitx5 {

class ImeEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit ImeEngine(fcitx::Instance* instance);
    ~ImeEngine() override;

    void keyEvent(const fcitx::InputMethodEntry& entry, fcitx::KeyEvent& event) override;
    void activate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reset(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration* getConfig() const override;
    void setConfig(const fcitx::RawConfig& config) override;

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
    void update_ui(fcitx::InputContext* input_context);
    void commit_current(fcitx::InputContext* input_context);
    bool select_candidate(fcitx::InputContext* input_context, int index);
    bool handle_escape(fcitx::InputContext* input_context);
    int candidate_page_size() const;
    int candidate_page_offset() const;
    bool page_candidates(int delta, bool preserve_cursor_offset = false);
    void reset_candidate_view();
    void show_candidate_ui();
    void hide_candidate_ui();
    void clamp_candidate_cursor();
    bool move_candidate_cursor_in_page(int delta);
    bool set_candidate_cursor(int index);
    bool candidate_ui_active() const;
    void mark_prediction_dirty();
    void apply_fallback_candidates(size_t segment_index);
    void request_prediction_if_ready(fcitx::InputContext* input_context);
    protocol::PredictRequest build_predict_request(const fcitx::InputContext* input_context) const;
    void send_prediction(fcitx::InputContext* input_context, std::uint64_t generation);
    void schedule_response(fcitx::InputContext* input_context, std::uint64_t generation, protocol::Message response);
    bool poll_prediction(fcitx::InputContext* input_context);
    std::vector<char32_t> current_candidates(bool include_hidden = false) const;
    CandidateTarget candidate_target_mode() const;
    std::optional<size_t> current_candidate_target() const;
    std::optional<int> selection_index_for_key(fcitx::KeySym key) const;
    fcitx::KeyList selection_key_list() const;
    fcitx::CandidateLayoutHint candidate_layout_hint() const;

    CompositionBuffer buffer_;
    FallbackEngine fallback_;
    ServiceTransport service_transport_;
    ImeFcitxConfig fcitx_config_;
    Config config_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    fcitx::Instance* instance_ = nullptr;
    fcitx::EventDispatcher* event_dispatcher_ = nullptr;
    bool prediction_pending_ = false;
    bool prediction_dirty_ = false;
    std::u16string prediction_key_;
    size_t prediction_revision_ = 0;
    std::vector<size_t> prediction_segment_indices_;
    std::vector<char32_t> displayed_candidates_;
    int candidate_page_ = 0;
    int candidate_cursor_ = 0;
    bool candidate_expanded_ = false;
    bool candidate_ui_hidden_ = true;

    protocol::SessionId session_id_{};
    std::uint64_t next_request_id_ = 1;
    std::uint64_t generation_ = 0;
    std::optional<std::uint64_t> inflight_request_id_;
    std::uint64_t inflight_revision_ = 0;
    fcitx::InputContext* active_input_context_ = nullptr;
    std::size_t state_scope_depth_ = 0;
    ImeInputContextPropertyFactory property_factory_;
};

class ImeEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override;
};

}  // namespace ime::fcitx5
