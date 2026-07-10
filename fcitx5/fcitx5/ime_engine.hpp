#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputmethodengine.h>

#include <memory>
#include <optional>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_client.hpp"
#include "fcitx5/ime_config.hpp"

namespace fcitx {
class EventDispatcher;
class Instance;
}  // namespace fcitx

namespace ime::fcitx5 {

class ImeEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit ImeEngine(fcitx::Instance* instance);

    void keyEvent(const fcitx::InputMethodEntry& entry, fcitx::KeyEvent& event) override;
    void activate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reset(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration* getConfig() const override;
    void setConfig(const fcitx::RawConfig& config) override;

private:
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
    void request_prediction_if_ready();
    PredictRequest build_predict_request() const;
    bool poll_prediction();
    std::vector<char32_t> current_candidates(bool include_hidden = false) const;
    CandidateTarget candidate_target_mode() const;
    std::optional<size_t> current_candidate_target() const;
    std::optional<int> selection_index_for_key(fcitx::KeySym key) const;
    fcitx::KeyList selection_key_list() const;
    fcitx::CandidateLayoutHint candidate_layout_hint() const;

    CompositionBuffer buffer_;
    FallbackEngine fallback_;
    ServiceClient service_client_;
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
};

class ImeEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override;
};

}  // namespace ime::fcitx5
