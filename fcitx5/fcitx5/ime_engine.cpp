#include "fcitx5/ime_engine.hpp"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include "bopomofo/keymap.hpp"
#include "input/keypad.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

const char* non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

std::string to_utf8(char32_t value) {
    return char32_to_utf8(value);
}

std::string to_utf8(const std::u16string& value) {
    return u16_to_utf8(value);
}

std::filesystem::path default_table_path() {
    if (const char* override = non_empty_env("IME_FCITX5_TABLE_PATH")) return override;
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_path =
            std::filesystem::path(home) / "Library" / "fcitx5" / "share" / "llavon-ime" / "tables" /
            "bopomofo_char.json";
        if (std::filesystem::exists(user_path)) return user_path;
    }
#endif
#ifdef IME_FCITX5_SOURCE_DIR
    const auto source_path = std::filesystem::path(IME_FCITX5_SOURCE_DIR) / ".." / "tables" / "bopomofo_char.json";
    if (std::filesystem::exists(source_path)) return source_path;
#endif
#ifdef IME_FCITX5_INSTALLED_TABLE_PATH
    const auto installed_path = std::filesystem::path(IME_FCITX5_INSTALLED_TABLE_PATH);
    if (std::filesystem::exists(installed_path)) return installed_path;
    return installed_path;
#endif
    return "/usr/share/llavon-ime/tables/bopomofo_char.json";
}

bool is_tone_key(char32_t value) {
    return value == U' ' || value == U'ˊ' || value == U'ˇ' || value == U'ˋ' || value == U'˙';
}

char32_t normalize_ascii_letter(char32_t key) {
    if (key >= U'A' && key <= U'Z') return key + (U'a' - U'A');
    return key;
}

bool has_blocking_modifier(const fcitx::Key& key) {
    return static_cast<bool>(key.states() & fcitx::KeyState::SimpleMask);
}

char32_t shifted_ascii_key(fcitx::KeySym key) {
    switch (key) {
        case FcitxKey_1:
            return U'!';
        case FcitxKey_2:
            return U'@';
        case FcitxKey_3:
            return U'#';
        case FcitxKey_4:
            return U'$';
        case FcitxKey_5:
            return U'%';
        case FcitxKey_6:
            return U'^';
        case FcitxKey_7:
            return U'&';
        case FcitxKey_8:
            return U'*';
        case FcitxKey_9:
            return U'(';
        case FcitxKey_0:
            return U')';
        case FcitxKey_minus:
            return U'_';
        case FcitxKey_equal:
            return U'+';
        case FcitxKey_semicolon:
            return U':';
        case FcitxKey_apostrophe:
            return U'"';
        case FcitxKey_comma:
            return U'<';
        case FcitxKey_period:
            return U'>';
        case FcitxKey_slash:
            return U'?';
        default:
            return static_cast<char32_t>(key);
    }
}

std::optional<char32_t> microsoft_punctuation_for_key(const fcitx::Key& key) {
    if ((key.states() & fcitx::KeyState::Alt) || (key.states() & fcitx::KeyState::Super)) return std::nullopt;
    const char32_t raw_symbol = static_cast<char32_t>(key.sym());
    const char32_t shifted_symbol = shifted_ascii_key(key.sym());
    const char32_t symbol = (key.states() & fcitx::KeyState::Shift) ? shifted_symbol : raw_symbol;

    if (key.states() & fcitx::KeyState::Ctrl) {
        if (const auto punctuation = lookup_microsoft_ctrl_punctuation_key(symbol)) return punctuation;
        return lookup_microsoft_ctrl_punctuation_key(raw_symbol);
    }

    return lookup_microsoft_punctuation_key(symbol);
}

std::u16string spaced_readings(const CompositionBuffer& buffer) {
    std::u16string result;
    for (const auto& segment : buffer.segments()) {
        if (!result.empty()) result.push_back(u' ');
        result += segment.reading();
    }
    return result;
}

class SelectableCandidateWord final : public fcitx::CandidateWord {
public:
    SelectableCandidateWord(fcitx::Text text, fcitx::Text comment, std::function<void(fcitx::InputContext*)> callback)
        : CandidateWord(std::move(text)), callback_(std::move(callback)) {
        if (!comment.empty()) setComment(std::move(comment));
    }

    SelectableCandidateWord(fcitx::Text text, std::function<void(fcitx::InputContext*)> callback)
        : SelectableCandidateWord(std::move(text), fcitx::Text(), std::move(callback)) {}

    void select(fcitx::InputContext* input_context) const override {
        callback_(input_context);
    }

private:
    std::function<void(fcitx::InputContext*)> callback_;
};

}  // namespace

ImeEngine::ImeEngine(fcitx::Instance* instance)
    : fallback_(default_table_path()),
      config_(default_config()),
      instance_(instance),
      event_dispatcher_(instance ? &instance->eventDispatcher() : nullptr) {
    reload_config();
}

void ImeEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) {
    if (event.isRelease()) return;

    auto* input_context = event.inputContext();
    const auto key = event.key().sym();
    const auto microsoft_punctuation = microsoft_punctuation_for_key(event.key());
    if (!microsoft_punctuation && has_blocking_modifier(event.key())) return;

    if (is_keypad_passthrough_keysym(static_cast<std::uint32_t>(key))) {
        if (!buffer_.empty()) commit_current(input_context);
        return;
    }

    if (poll_prediction()) update_ui(input_context);

    if (candidate_ui_active()) {
        if (key == FcitxKey_Up) {
            if (move_candidate_cursor_in_page(-1)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_Down) {
            if (move_candidate_cursor_in_page(1)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_Left) {
            if (page_candidates(-1, true)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_Right) {
            if (page_candidates(1, true)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_Home) {
            if (set_candidate_cursor(0)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_End) {
            if (set_candidate_cursor(static_cast<int>(displayed_candidates_.size()) - 1)) update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (is_return_keysym(static_cast<std::uint32_t>(key))) {
            (void)select_candidate(input_context, candidate_cursor_);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_space && config_.space_selects_candidate) {
            (void)select_candidate(input_context, candidate_cursor_);
            event.filterAndAccept();
            return;
        }
    }

    if (microsoft_punctuation) {
        if (!buffer_.has_unfinished_reading()) {
            (void)buffer_.add_literal(*microsoft_punctuation);
            hide_candidate_ui();
            reset_candidate_view();
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    if (const auto index = selection_index_for_key(key); index && candidate_ui_active()) {
        (void)select_candidate(input_context, candidate_page_offset() + *index);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_space && !buffer_.empty()) {
        const bool target_complete = current_candidate_target().has_value();
        const bool has_candidates = !current_candidates(true).empty();
        if (target_complete || has_candidates) {
            if (candidate_ui_hidden_) {
                reset_candidate_view();
                show_candidate_ui();
                update_ui(input_context);
            } else if (config_.space_selects_candidate && !displayed_candidates_.empty()) {
                (void)select_candidate(input_context, candidate_cursor_);
            }
            event.filterAndAccept();
            return;
        }
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key)) && !buffer_.empty()) {
        commit_current(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Escape && !buffer_.empty()) {
        (void)handle_escape(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_BackSpace && !buffer_.empty()) {
        buffer_.backspace();
        hide_candidate_ui();
        reset_candidate_view();
        mark_prediction_dirty();
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Left && !buffer_.empty()) {
        if (buffer_.move_cursor_left()) {
            hide_candidate_ui();
            reset_candidate_view();
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Right && !buffer_.empty()) {
        if (buffer_.move_cursor_right()) {
            hide_candidate_ui();
            reset_candidate_view();
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Down && !buffer_.empty()) {
        const bool target_complete = current_candidate_target().has_value();
        const bool has_candidates = !current_candidates(true).empty();
        if (target_complete || has_candidates) {
            reset_candidate_view();
            show_candidate_ui();
            update_ui(input_context);
            event.filterAndAccept();
            return;
        }
    }

    if (key == FcitxKey_Tab && !buffer_.empty() && !current_candidates(true).empty()) {
        if (candidate_ui_hidden_) {
            reset_candidate_view();
            show_candidate_ui();
        } else {
            candidate_expanded_ = !candidate_expanded_;
        }
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    if ((key == FcitxKey_Page_Up || key == FcitxKey_Page_Down) && !buffer_.empty() && !displayed_candidates_.empty()) {
        if (page_candidates(key == FcitxKey_Page_Up ? -1 : 1)) {
            (void)set_candidate_cursor(candidate_page_offset());
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    const auto mapped = lookup_bopomofo_key(static_cast<char32_t>(key), config_.caps_lock_inputs_bopomofo);
    if (key == FcitxKey_space && buffer_.empty()) return;
    if (mapped && buffer_.add_bopomofo(*mapped)) {
        hide_candidate_ui();
        reset_candidate_view();
        mark_prediction_dirty();
        if (is_tone_key(*mapped)) {
            if (const auto segment = buffer_.last_edited_segment(); segment && buffer_.segment_complete(*segment)) {
                apply_fallback_candidates(*segment);
                const auto* candidates = buffer_.segment_candidates(*segment);
                if (candidates == nullptr || candidates->empty()) {
                    (void)buffer_.remove_segment(*segment);
                    update_ui(input_context);
                    event.filterAndAccept();
                    return;
                }
            }
            request_prediction_if_ready();
        }
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }
}

void ImeEngine::activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    reload_config();
    update_ui(event.inputContext());
}

void ImeEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    if (event.type() != fcitx::EventType::InputContextFocusOut && event.type() != fcitx::EventType::InputContextReset) {
        const auto text = buffer_.candidate_commit_text();
        if (!text.empty()) event.inputContext()->commitString(to_utf8(text));
    }

    buffer_.clear();
    displayed_candidates_.clear();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    prediction_segment_indices_.clear();
    hide_candidate_ui();
    reset_candidate_view();
    update_ui(event.inputContext());
}

void ImeEngine::reloadConfig() {
    reload_config();
}

void ImeEngine::save() {
    // PkgConfig maps to $XDG_CONFIG_HOME/fcitx5, matching shared config_path().
    fcitx::safeSaveAsIni(fcitx_config_, fcitx::StandardPathsType::PkgConfig, kFcitxConfigFile);
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &fcitx_config_;
}

void ImeEngine::setConfig(const fcitx::RawConfig& config) {
    fcitx_config_.load(config, true);
    config_ = to_shared_config(fcitx_config_);
    save();
    (void)service_client_.stop();
}

void ImeEngine::reload_config() {
    fcitx_config_ = ImeFcitxConfig();
    try {
        fcitx::readAsIni(fcitx_config_, fcitx::StandardPathsType::PkgConfig, kFcitxConfigFile);
    } catch (...) {
        fcitx_config_ = ImeFcitxConfig();
    }

    std::error_code ec;
    const bool has_fcitx_config = std::filesystem::exists(config_path(), ec) && !ec;
    apply_shared_config(fcitx_config_, load_config());
    if (!has_fcitx_config) save();
    config_ = to_shared_config(fcitx_config_);
}

void ImeEngine::update_ui(fcitx::InputContext* input_context) {
    (void)poll_prediction();

    if (buffer_.empty()) {
        displayed_candidates_.clear();
        reset_candidate_view();
        hide_candidate_ui();
        input_context->inputPanel().reset();
        input_context->updatePreedit();
        input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    fcitx::Text preedit(to_utf8(buffer_.rendered_composition()));
    preedit.setCursor(static_cast<int>(to_utf8(buffer_.rendered_prefix_before_caret()).size()));
    input_context->inputPanel().setClientPreedit(preedit);
    if (candidate_ui_hidden_) {
        input_context->inputPanel().setPreedit(fcitx::Text());
        input_context->inputPanel().setAuxUp(fcitx::Text());
        input_context->inputPanel().setAuxDown(fcitx::Text());
    } else {
        input_context->inputPanel().setPreedit(preedit);
        input_context->inputPanel().setAuxUp(fcitx::Text(to_utf8(spaced_readings(buffer_))));
        input_context->inputPanel().setAuxDown(fcitx::Text());
    }
    input_context->updatePreedit();

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    displayed_candidates_ = current_candidates();
    if (displayed_candidates_.empty()) {
        if (!candidate_ui_hidden_) {
            const auto target = current_candidate_target();
            if (target && buffer_.segment_complete(*target)) {
                input_context->inputPanel().setAuxDown(
                    fcitx::Text("No candidates: " + to_utf8(buffer_.segment_reading(*target))));
            }
        }
        reset_candidate_view();
        input_context->inputPanel().setCandidateList(nullptr);
        input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    clamp_candidate_cursor();
    const int page_size = candidate_page_size();
    const int page_count = static_cast<int>(
        (displayed_candidates_.size() + static_cast<size_t>(page_size) - 1) / static_cast<size_t>(page_size));
    if (candidate_page_ >= page_count) candidate_page_ = page_count - 1;
    if (candidate_page_ < 0) candidate_page_ = 0;
    candidates->setPageSize(page_size);
    candidates->setSelectionKey(selection_key_list());
    candidates->setLayoutHint(candidate_layout_hint());
    const auto target = current_candidate_target();
    const auto target_reading = target ? to_utf8(buffer_.segment_reading(*target)) : std::string();
    int index = 0;
    for (const auto candidate : displayed_candidates_) {
        candidates->append<SelectableCandidateWord>(
            fcitx::Text(to_utf8(candidate)), fcitx::Text(target_reading), [this, index](fcitx::InputContext* context) {
                select_candidate(context, index);
            });
        ++index;
    }
    candidates->setPage(candidate_page_);
    if (target) candidates->setCursorIndex(candidate_cursor_ - candidate_page_offset());
    input_context->inputPanel().setCandidateList(std::move(candidates));
    input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ImeEngine::commit_current(fcitx::InputContext* input_context) {
    input_context->commitString(to_utf8(buffer_.commit_text()));
    buffer_.clear();
    displayed_candidates_.clear();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    prediction_segment_indices_.clear();
    hide_candidate_ui();
    update_ui(input_context);
}

bool ImeEngine::select_candidate(fcitx::InputContext* input_context, int index) {
    const auto target = current_candidate_target();
    if (!target || index < 0) return false;

    const auto candidates = current_candidates();
    if (index >= static_cast<int>(candidates.size())) return false;

    if (!buffer_.select_candidate(*target, static_cast<size_t>(index), config_.move_cursor_after_selection))
        return false;
    hide_candidate_ui();
    mark_prediction_dirty();
    update_ui(input_context);
    return true;
}

bool ImeEngine::handle_escape(fcitx::InputContext* input_context) {
    if (config_.esc_clears_entire_buffer) {
        buffer_.clear();
        displayed_candidates_.clear();
        prediction_pending_ = false;
        prediction_dirty_ = false;
        hide_candidate_ui();
        update_ui(input_context);
        return true;
    }

    const auto target = current_candidate_target();
    if (!candidate_ui_hidden_ && !current_candidates(true).empty()) {
        candidate_ui_hidden_ = true;
        displayed_candidates_.clear();
        reset_candidate_view();
        update_ui(input_context);
        return true;
    }

    if (target && buffer_.cancel_candidate_selection(*target)) {
        hide_candidate_ui();
        reset_candidate_view();
        mark_prediction_dirty();
        update_ui(input_context);
        return true;
    }

    buffer_.clear();
    displayed_candidates_.clear();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    hide_candidate_ui();
    update_ui(input_context);
    return true;
}

int ImeEngine::candidate_page_size() const {
    if (candidate_expanded_ && !displayed_candidates_.empty()) return static_cast<int>(displayed_candidates_.size());
    return config_.candidate_page_size;
}

int ImeEngine::candidate_page_offset() const {
    return candidate_page_ * candidate_page_size();
}

bool ImeEngine::page_candidates(int delta, bool preserve_cursor_offset) {
    if (displayed_candidates_.empty()) return false;

    const int page_size = candidate_page_size();
    const int page_count = static_cast<int>(
        (displayed_candidates_.size() + static_cast<size_t>(page_size) - 1) / static_cast<size_t>(page_size));
    const int next_page = std::clamp(candidate_page_ + delta, 0, page_count - 1);
    if (next_page == candidate_page_) return false;

    const int cursor_offset = preserve_cursor_offset ? candidate_cursor_ % page_size : 0;
    candidate_page_ = next_page;
    if (preserve_cursor_offset) {
        const int page_begin = candidate_page_offset();
        const int max_index = static_cast<int>(displayed_candidates_.size()) - 1;
        candidate_cursor_ = std::min(page_begin + cursor_offset, max_index);
    }
    return true;
}

void ImeEngine::reset_candidate_view() {
    candidate_page_ = 0;
    candidate_cursor_ = 0;
    candidate_expanded_ = false;
}

void ImeEngine::show_candidate_ui() {
    if (candidate_ui_hidden_) {
        candidate_cursor_ = 0;
        if (const auto target = current_candidate_target()) {
            if (const auto selected = buffer_.segment_selected_index(*target)) {
                candidate_cursor_ = static_cast<int>(*selected);
            }
        }
    }
    candidate_ui_hidden_ = false;
    clamp_candidate_cursor();
}

void ImeEngine::hide_candidate_ui() {
    candidate_ui_hidden_ = true;
    candidate_cursor_ = 0;
}

void ImeEngine::clamp_candidate_cursor() {
    if (displayed_candidates_.empty()) {
        candidate_cursor_ = 0;
        candidate_page_ = 0;
        return;
    }

    const int max_index = static_cast<int>(displayed_candidates_.size()) - 1;
    candidate_cursor_ = std::clamp(candidate_cursor_, 0, max_index);

    const int page_size = candidate_page_size();
    if (page_size > 0) candidate_page_ = candidate_cursor_ / page_size;
}

bool ImeEngine::move_candidate_cursor_in_page(int delta) {
    if (displayed_candidates_.empty()) return false;

    const int page_size = candidate_page_size();
    if (page_size <= 0) return false;

    const int page_begin = candidate_page_offset();
    const int page_end = std::min(page_begin + page_size, static_cast<int>(displayed_candidates_.size()));
    if (page_begin >= page_end) return false;

    int next = candidate_cursor_ + delta;
    if (next < page_begin) next = page_end - 1;
    if (next >= page_end) next = page_begin;
    if (next == candidate_cursor_) return false;

    candidate_cursor_ = next;
    return true;
}

bool ImeEngine::set_candidate_cursor(int index) {
    if (displayed_candidates_.empty()) return false;

    const int max_index = static_cast<int>(displayed_candidates_.size()) - 1;
    const int next = std::clamp(index, 0, max_index);
    if (next == candidate_cursor_) return false;

    candidate_cursor_ = next;
    clamp_candidate_cursor();
    return true;
}

bool ImeEngine::candidate_ui_active() const {
    return !candidate_ui_hidden_ && !displayed_candidates_.empty();
}

void ImeEngine::mark_prediction_dirty() {
    if (prediction_pending_) prediction_dirty_ = true;
}

void ImeEngine::apply_fallback_candidates(size_t segment_index) {
    if (!buffer_.segment_complete(segment_index)) return;

    const auto predictions = fallback_.predict(buffer_);
    if (segment_index >= predictions.size()) return;
    (void)buffer_.set_segment_candidates(segment_index, predictions[segment_index].candidates, false);
}

void ImeEngine::request_prediction_if_ready() {
    if (prediction_pending_) {
        prediction_dirty_ = true;
        return;
    }
    auto request = build_predict_request();
    if (request.padding.empty()) return;
    prediction_segment_indices_ = buffer_.completed_segment_indices();
    prediction_pending_ = true;
    prediction_dirty_ = false;
    prediction_key_ = buffer_.raw_composition();
    prediction_revision_ = buffer_.revision();
    const auto alive = std::weak_ptr<bool>(alive_);
    auto* dispatcher = event_dispatcher_;
    (void)service_client_.request_predict_async(std::move(request), [this, alive, dispatcher](PredictState) {
        if (!dispatcher || alive.expired()) return;
        dispatcher->schedule([this, alive]() {
            if (alive.expired() || !instance_) return;
            auto* input_context = instance_->inputContextManager().lastFocusedInputContext();
            if (input_context) update_ui(input_context);
        });
    });
}

PredictRequest ImeEngine::build_predict_request() const {
    PredictRequest request;
    for (const auto& segment : buffer_.segments()) {
        if (!segment.complete()) continue;

        PaddingEntry entry;
        entry.bopomofo = segment.reading();
        if (segment.manually_chosen && segment.selected_candidate() != 0) {
            entry.chosen = true;
            entry.chosen_char = segment.selected_candidate();
        }
        request.padding.push_back(std::move(entry));
    }
    return request;
}

bool ImeEngine::poll_prediction() {
    if (prediction_pending_) {
        if (auto response = service_client_.latest_response()) {
            prediction_pending_ = false;
            bool changed = false;
            if (prediction_key_ == buffer_.raw_composition() && prediction_revision_ == buffer_.revision()) {
                const size_t count = std::min(response->candidates.size(), prediction_segment_indices_.size());
                for (size_t i = 0; i < count; ++i) {
                    if (!response->candidates[i].empty()) {
                        changed =
                            buffer_.set_segment_candidates(prediction_segment_indices_[i], response->candidates[i]) ||
                            changed;
                    }
                }
            }
            if (prediction_dirty_) {
                prediction_dirty_ = false;
                request_prediction_if_ready();
            }
            return changed;
        } else if (service_client_.state() == PredictState::Unavailable) {
            prediction_pending_ = false;
            if (prediction_dirty_) {
                prediction_dirty_ = false;
                request_prediction_if_ready();
            }
            return true;
        }
    }

    return false;
}

std::vector<char32_t> ImeEngine::current_candidates(bool include_hidden) const {
    if (candidate_ui_hidden_ && !include_hidden) return {};

    const auto target = current_candidate_target();
    if (!target) return {};

    const auto* candidates = buffer_.segment_candidates(*target);
    if (candidates == nullptr) return {};
    return *candidates;
}

CandidateTarget ImeEngine::candidate_target_mode() const {
    return config_.select_phrase == "after_cursor" ? CandidateTarget::AfterCursor : CandidateTarget::BeforeCursor;
}

std::optional<size_t> ImeEngine::current_candidate_target() const {
    return buffer_.candidate_target(candidate_target_mode());
}

std::optional<int> ImeEngine::selection_index_for_key(fcitx::KeySym key) const {
    const auto raw_key = static_cast<char32_t>(key);
    const auto normalized_key = config_.caps_lock_inputs_bopomofo ? normalize_ascii_letter(raw_key) : raw_key;
    const int count = std::min(config_.selection_key_count, static_cast<int>(config_.selection_keys.size()));
    for (int i = 0; i < count; ++i) {
        const auto selection_key =
            static_cast<char32_t>(static_cast<unsigned char>(config_.selection_keys[static_cast<size_t>(i)]));
        if (normalized_key == selection_key) return i;
    }
    return std::nullopt;
}

fcitx::KeyList ImeEngine::selection_key_list() const {
    fcitx::KeyList keys;
    const int count = std::min(config_.selection_key_count, static_cast<int>(config_.selection_keys.size()));
    keys.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        keys.emplace_back(
            static_cast<fcitx::KeySym>(static_cast<unsigned char>(config_.selection_keys[static_cast<size_t>(i)])));
    }
    return keys;
}

fcitx::CandidateLayoutHint ImeEngine::candidate_layout_hint() const {
    if (config_.candidate_layout == "vertical") return fcitx::CandidateLayoutHint::Vertical;
    if (config_.candidate_layout == "horizontal") return fcitx::CandidateLayoutHint::Horizontal;
    return fcitx::CandidateLayoutHint::NotSet;
}

fcitx::AddonInstance* ImeEngineFactory::create(fcitx::AddonManager* manager) {
    return new ImeEngine(manager ? manager->instance() : nullptr);
}

}  // namespace ime::fcitx5

FCITX_ADDON_FACTORY(ime::fcitx5::ImeEngineFactory)
