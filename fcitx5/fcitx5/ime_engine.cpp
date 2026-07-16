#include "fcitx5/ime_engine.hpp"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#if __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define IME_FCITX5_MODERN_STANDARD_PATHS 1
#else
#include <fcitx-utils/standardpath.h>
#endif
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <utility>

#include "bopomofo/keymap.hpp"
#include "input/keypad.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

#ifdef IME_FCITX5_MODERN_STANDARD_PATHS
constexpr auto kFcitxConfigPathType = fcitx::StandardPathsType::PkgConfig;
#else
constexpr auto kFcitxConfigPathType = fcitx::StandardPath::Type::PkgConfig;
#endif

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

ServiceTransportOptions default_transport_options() {
    ServiceTransportOptions options;
    const auto config = load_config();
    options.tables_dir = default_table_path().parent_path();
    options.model_path = config.model_path;
    options.context_length = static_cast<std::uint32_t>(config.context_length);
    options.threads = static_cast<std::uint32_t>(config.thread_count);
    options.gpu_layers = config.gpu_layers;
    options.idle_timeout_seconds = static_cast<std::uint32_t>(config.idle_timeout_seconds);
    options.personal_learning_enabled = config.personal_learning_enabled;
    options.lora_training_enabled = config.lora_training_enabled;
    options.training_base_safetensors_path = config.training_base_safetensors_path;
    options.base_model_sha256 = config.base_model_sha256;
    if (const char* model = non_empty_env("IME_FCITX5_MODEL_PATH")) options.model_path = model;
    return options;
}

bool is_tone_key(char32_t value) {
    return value == U' ' || value == U'ˊ' || value == U'ˇ' || value == U'ˋ' || value == U'˙';
}

bool is_semantic_latin(char32_t value) {
    return (value >= U'a' && value <= U'z') || (value >= U'A' && value <= U'Z') ||
           (value >= U'0' && value <= U'9') || value == U'-' || value == U'_' || value == U'+';
}

// This mirrors the runtime tokenizer's context grouping: each CJK/space/unknown
// scalar is one token and a contiguous ASCII latin run is one latin token.
std::size_t trim_context_to_token_budget(std::u32string& context, std::size_t budget) {
    auto next_token_end = [&context](std::size_t begin) {
        if (begin >= context.size()) return begin;
        if (!is_semantic_latin(context[begin])) return begin + 1U;
        std::size_t end = begin + 1U;
        while (end < context.size() && is_semantic_latin(context[end])) ++end;
        return end;
    };

    std::size_t tokens = 0;
    for (std::size_t index = 0; index < context.size();) {
        index = next_token_end(index);
        ++tokens;
    }
    std::size_t first = 0;
    while (tokens > budget && first < context.size()) {
        first = next_token_end(first);
        --tokens;
    }
    if (first != 0) context.erase(0, first);
    return tokens;
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

void append_utf16_scalar(std::u16string& output, char32_t scalar) {
    if (scalar <= 0xffffU) {
        output.push_back(static_cast<char16_t>(scalar));
        return;
    }
    scalar -= 0x10000U;
    output.push_back(static_cast<char16_t>(0xd800U + (scalar >> 10U)));
    output.push_back(static_cast<char16_t>(0xdc00U + (scalar & 0x3ffU)));
}

protocol::EventId new_feedback_event_id() {
    protocol::EventId result{};
    try {
        std::random_device random;
        for (auto& byte : result) byte = static_cast<std::uint8_t>(random());
    } catch (...) {
        static std::uint64_t sequence = 0;
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^ ++sequence;
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<std::uint8_t>(seed >> ((index % 8U) * 8U));
        }
    }
    if (protocol::is_zero(result)) result[0] = 1;
    return result;
}

class SelectableCandidateWord final : public fcitx::CandidateWord {
public:
    SelectableCandidateWord(fcitx::Text text, fcitx::Text comment, std::function<void(fcitx::InputContext*)> callback)
        : CandidateWord(std::move(text)), callback_(std::move(callback)) {
        (void)comment;
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

ImeEngine::StateScope::StateScope(ImeEngine& engine, fcitx::InputContext* input_context) : engine_(engine) {
    if (input_context != nullptr) {
        engine_.enter_context(input_context);
        entered_ = true;
    }
}

ImeEngine::StateScope::~StateScope() {
    if (entered_) engine_.leave_context();
}

ImeInputContextProperty* ImeEngine::property(fcitx::InputContext* input_context) const {
    if (input_context == nullptr) return nullptr;
    return static_cast<ImeInputContextProperty*>(input_context->property(&property_factory_));
}

void ImeEngine::enter_context(fcitx::InputContext* input_context) {
    if (state_scope_depth_++ != 0) return;
    active_input_context_ = input_context;
    auto* state = property(input_context);
    if (state == nullptr) return;

    buffer_ = state->buffer;
    displayed_candidates_ = state->displayed_candidates;
    candidate_page_ = state->candidate_page;
    candidate_cursor_ = state->candidate_cursor;
    candidate_expanded_ = state->candidate_expanded;
    candidate_ui_hidden_ = state->candidate_ui_hidden;
    session_id_ = state->session_id;
    next_request_id_ = state->next_request_id;
    generation_ = state->generation;
    inflight_request_id_ = state->inflight_request_id;
    inflight_revision_ = state->inflight_revision;
    prediction_key_ = state->prediction_key;
    prediction_context_ = state->prediction_context;
    prediction_base_model_hash_ = state->prediction_base_model_hash;
    prediction_feedback_token_ = state->prediction_feedback_token;
    feedback_sensitive_ = state->feedback_sensitive;
    prediction_segment_indices_ = state->inflight_segment_indices;
    prediction_pending_ = state->prediction_pending;
    prediction_dirty_ = state->prediction_dirty;
}

void ImeEngine::leave_context() {
    if (state_scope_depth_ == 0) return;
    if (--state_scope_depth_ != 0) return;
    auto* state = property(active_input_context_);
    if (state != nullptr) {
        state->buffer = buffer_;
        state->displayed_candidates = displayed_candidates_;
        state->candidate_page = candidate_page_;
        state->candidate_cursor = candidate_cursor_;
        state->candidate_expanded = candidate_expanded_;
        state->candidate_ui_hidden = candidate_ui_hidden_;
        state->session_id = session_id_;
        state->next_request_id = next_request_id_;
        state->generation = generation_;
        state->inflight_request_id = inflight_request_id_;
        state->inflight_revision = inflight_revision_;
        state->prediction_key = prediction_key_;
        state->prediction_context = prediction_context_;
        state->prediction_base_model_hash = prediction_base_model_hash_;
        state->prediction_feedback_token = prediction_feedback_token_;
        state->feedback_sensitive = feedback_sensitive_;
        state->inflight_segment_indices = prediction_segment_indices_;
        state->prediction_pending = prediction_pending_;
        state->prediction_dirty = prediction_dirty_;
    }
    active_input_context_ = nullptr;
}

ImeEngine::ImeEngine(fcitx::Instance* instance)
    : fallback_(default_table_path()),
      service_transport_(std::make_unique<ServiceTransport>(default_transport_options())),
      config_(default_config()),
      instance_(instance) {
    if (instance_ != nullptr) {
        event_dispatcher_ = std::make_unique<fcitx::EventDispatcher>();
        event_dispatcher_->attach(&instance_->eventLoop());
        (void)instance_->inputContextManager().registerProperty("llavon-ime-input-state", &property_factory_);
    }
    reload_config();
}

ImeEngine::~ImeEngine() {
    if (alive_) *alive_ = false;
    if (service_transport_) service_transport_->stop();
}

void ImeEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) {
    if (event.isRelease()) return;

    auto* input_context = event.inputContext();
    StateScope state_scope(*this, input_context);
    if (input_context != nullptr &&
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        feedback_sensitive_ = true;
        prediction_context_.clear();
        prediction_base_model_hash_.clear();
        prediction_feedback_token_ = {};
    }
    const auto key = event.key().sym();
    const auto microsoft_punctuation = microsoft_punctuation_for_key(event.key());
    if (!microsoft_punctuation && has_blocking_modifier(event.key())) return;

    if (is_keypad_passthrough_keysym(static_cast<std::uint32_t>(key))) {
        if (!buffer_.empty()) commit_current(input_context);
        return;
    }

    if (poll_prediction(input_context)) update_ui(input_context);

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

    if (key == FcitxKey_Delete && !buffer_.empty()) {
        buffer_.delete_forward();
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
            request_prediction_if_ready(input_context);
        }
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }
}

void ImeEngine::activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    reload_config();
    StateScope state_scope(*this, event.inputContext());
    update_ui(event.inputContext());
}

void ImeEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    StateScope state_scope(*this, event.inputContext());
    if (event.type() != fcitx::EventType::InputContextFocusOut && event.type() != fcitx::EventType::InputContextReset) {
        const auto text = buffer_.candidate_commit_text();
        if (!text.empty()) event.inputContext()->commitString(to_utf8(text));
    }

    buffer_.clear();
    prediction_context_.clear();
    prediction_base_model_hash_.clear();
    prediction_feedback_token_ = {};
    feedback_sensitive_ = false;
    displayed_candidates_.clear();
    ++generation_;
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
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
    fcitx::safeSaveAsIni(fcitx_config_, kFcitxConfigPathType, kFcitxConfigFile);
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &fcitx_config_;
}

void ImeEngine::setConfig(const fcitx::RawConfig& config) {
    const auto previous = config_;
    fcitx_config_.load(config, true);
    (void)fcitx_config_.version.setValue(DisplayVersion::Current);
    const bool delete_requested = *fcitx_config_.deletePersonalData;
    if (delete_requested) {
        (void)fcitx_config_.deletePersonalData.setValue(true);
        (void)fcitx_config_.personalLearningEnabled.setValue(previous.personal_learning_enabled);
        (void)fcitx_config_.loraTrainingEnabled.setValue(previous.lora_training_enabled);
        config_ = to_shared_config(fcitx_config_);
        save();
        request_personal_data_deletion();
        return;
    }
    config_ = to_shared_config(fcitx_config_);
    save();
    ++generation_;
    inflight_request_id_.reset();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    if (instance_ != nullptr) {
        instance_->inputContextManager().foreach([this](fcitx::InputContext* input_context) {
            auto* state = property(input_context);
            if (state == nullptr) return true;
            if (!protocol::is_zero(state->session_id)) {
                service_transport_->close_session(state->session_id, {});
                state->session_id = {};
            }
            state->session_close_handle = {};
            state->invalidate_generation();
            return true;
        });
    }
    rebuild_service_transport();
}

void ImeEngine::reload_config() {
    const auto previous = config_;
    fcitx_config_ = ImeFcitxConfig();
    try {
        fcitx::readAsIni(fcitx_config_, kFcitxConfigPathType, kFcitxConfigFile);
    } catch (...) {
        fcitx_config_ = ImeFcitxConfig();
    }

    std::error_code ec;
    const bool has_fcitx_config = std::filesystem::exists(config_path(), ec) && !ec;
    apply_shared_config(fcitx_config_, load_config());
    const bool delete_requested = *fcitx_config_.deletePersonalData;
    if (delete_requested) {
        const bool persisted_personal_learning = *fcitx_config_.personalLearningEnabled;
        const bool persisted_lora_training = *fcitx_config_.loraTrainingEnabled;
        (void)fcitx_config_.deletePersonalData.setValue(true);
        (void)fcitx_config_.personalLearningEnabled.setValue(persisted_personal_learning);
        (void)fcitx_config_.loraTrainingEnabled.setValue(persisted_lora_training);
        save();
    }
    if (!has_fcitx_config) save();
    config_ = to_shared_config(fcitx_config_);
    if (delete_requested) {
        request_personal_data_deletion();
        return;
    }
    if (previous.model_path != config_.model_path || previous.context_length != config_.context_length ||
         previous.thread_count != config_.thread_count || previous.gpu_layers != config_.gpu_layers ||
         previous.idle_timeout_seconds != config_.idle_timeout_seconds ||
         previous.personal_learning_enabled != config_.personal_learning_enabled ||
         previous.lora_training_enabled != config_.lora_training_enabled ||
         previous.training_base_safetensors_path != config_.training_base_safetensors_path ||
         previous.base_model_sha256 != config_.base_model_sha256) {
         if (instance_ != nullptr) {
            instance_->inputContextManager().foreach([this](fcitx::InputContext* input_context) {
                auto* state = property(input_context);
                if (state == nullptr) return true;
                if (!protocol::is_zero(state->session_id)) {
                    service_transport_->close_session(state->session_id, {});
                    state->session_id = {};
                }
                state->session_close_handle = {};
                state->invalidate_generation();
                return true;
            });
        }
        rebuild_service_transport();
    }
}

void ImeEngine::rebuild_service_transport() {
    auto previous = std::move(service_transport_);
    if (previous) previous->stop();
    service_transport_ = std::make_unique<ServiceTransport>(default_transport_options());
}

void ImeEngine::request_personal_data_deletion() {
    if (deletion_in_flight_ || !service_transport_) return;
    deletion_in_flight_ = true;
    const auto engine_alive = std::weak_ptr<bool>(alive_);
    auto* dispatcher = event_dispatcher_.get();
    service_transport_->delete_personal_data(
        [this, engine_alive, dispatcher](protocol::Message response) mutable {
            if (engine_alive.expired() || dispatcher == nullptr) return;
            const auto* result = std::get_if<protocol::DeletePersonalDataResponse>(&response);
            const bool deleted = result != nullptr && result->deleted;
            dispatcher->schedule([this, engine_alive, deleted]() {
                if (engine_alive.expired()) return;
                finish_personal_data_deletion(deleted);
            });
        });
}

void ImeEngine::finish_personal_data_deletion(bool deleted) {
    deletion_in_flight_ = false;
    if (!deleted) {
        // Keep the one-shot flag and previous opt-in state so a later reload can retry.
        save();
        return;
    }
    (void)fcitx_config_.deletePersonalData.setValue(false);
    (void)fcitx_config_.personalLearningEnabled.setValue(false);
    (void)fcitx_config_.loraTrainingEnabled.setValue(false);
    config_ = to_shared_config(fcitx_config_);
    save();
    ++generation_;
    inflight_request_id_.reset();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    if (instance_ != nullptr) {
        instance_->inputContextManager().foreach([this](fcitx::InputContext* input_context) {
            auto* state = property(input_context);
            if (state == nullptr) return true;
            if (!protocol::is_zero(state->session_id)) service_transport_->close_session(state->session_id, {});
            state->session_id = {};
            state->session_close_handle = {};
            state->invalidate_generation();
            return true;
        });
    }
    rebuild_service_transport();
}

void ImeEngine::update_ui(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    (void)poll_prediction(input_context);

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
    if (target) candidates->setGlobalCursorIndex(candidate_cursor_);
    input_context->inputPanel().setCandidateList(std::move(candidates));
    input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ImeEngine::commit_current(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    submit_feedback_if_eligible(input_context);
    input_context->commitString(to_utf8(buffer_.commit_text()));
    buffer_.clear();
    prediction_context_.clear();
    prediction_base_model_hash_.clear();
    prediction_feedback_token_ = {};
    feedback_sensitive_ = false;
    displayed_candidates_.clear();
    prediction_pending_ = false;
    prediction_dirty_ = false;
    prediction_segment_indices_.clear();
    hide_candidate_ui();
    update_ui(input_context);
}

void ImeEngine::submit_feedback_if_eligible(fcitx::InputContext* input_context) {
    if (!config_.personal_learning_enabled || prediction_base_model_hash_.empty() || feedback_sensitive_ || input_context == nullptr ||
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return;
    }

    protocol::FeedbackRequest feedback;
    feedback.event_id = new_feedback_event_id();
    feedback.session_id = session_id_;
    feedback.feedback_token = prediction_feedback_token_;
    feedback.left_context = prediction_context_;
    feedback.base_model_hash = prediction_base_model_hash_;
    feedback.created_at = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    bool saw_manual_selection = false;
    bool saw_correction = false;
    bool used_fallback = false;
    for (const auto& segment : buffer_.segments()) {
        if (!segment.complete() || segment.literal != 0 || segment.candidates.empty() ||
            segment.selected_index >= segment.candidates.size() || segment.candidate_source == CandidateSource::None) {
            return;
        }
        const auto selected = segment.selected_candidate();
        if (selected == 0 || std::find(segment.candidates.begin(), segment.candidates.end(), selected) == segment.candidates.end()) {
            return;
        }
        if (!feedback.bopomofo_sequence.empty()) feedback.bopomofo_sequence.push_back(protocol::kBopomofoReadingSeparator);
        feedback.bopomofo_sequence += segment.reading();
        append_utf16_scalar(feedback.committed_characters, selected);
        append_utf16_scalar(feedback.predicted_top1, segment.candidates.front());
        feedback.manually_chosen_flags.push_back(segment.manually_chosen);
        saw_manual_selection = saw_manual_selection || segment.manually_chosen;
        saw_correction = saw_correction || (segment.manually_chosen && segment.selected_index != 0);
        used_fallback = used_fallback || segment.candidate_source == CandidateSource::Fallback;
    }
    if (feedback.committed_characters.empty() || used_fallback) return;
    if (saw_correction) {
        feedback.signal_type = protocol::FeedbackSignal::ExplicitCorrection;
    } else if (saw_manual_selection) {
        feedback.signal_type = protocol::FeedbackSignal::ExplicitTop1Selection;
    } else {
        feedback.signal_type = protocol::FeedbackSignal::AcceptedPrediction;
    }
    service_transport_->submit_feedback(std::move(feedback), {});
}

bool ImeEngine::select_candidate(fcitx::InputContext* input_context, int index) {
    StateScope state_scope(*this, input_context);
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
    StateScope state_scope(*this, input_context);
    if (config_.esc_clears_entire_buffer) {
        buffer_.clear();
        prediction_context_.clear();
        prediction_base_model_hash_.clear();
        prediction_feedback_token_ = {};
        feedback_sensitive_ = false;
        displayed_candidates_.clear();
        ++generation_;
        prediction_pending_ = false;
        prediction_dirty_ = false;
        inflight_request_id_.reset();
        prediction_segment_indices_.clear();
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
    prediction_context_.clear();
    prediction_base_model_hash_.clear();
    prediction_feedback_token_ = {};
    feedback_sensitive_ = false;
    displayed_candidates_.clear();
    ++generation_;
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
    prediction_segment_indices_.clear();
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
    (void)buffer_.set_segment_candidates(segment_index, predictions[segment_index].candidates);
}

void ImeEngine::request_prediction_if_ready(fcitx::InputContext* input_context) {
    if (prediction_pending_) {
        prediction_dirty_ = true;
        return;
    }
    const auto request = build_predict_request(input_context);
    if (request.padding.empty()) return;
    if (config_.context_length < 2 ||
        request.padding.size() > static_cast<std::size_t>((config_.context_length - 2) / 2)) return;
    prediction_segment_indices_ = buffer_.completed_segment_indices();
    prediction_pending_ = true;
    prediction_dirty_ = false;
    prediction_key_ = buffer_.raw_composition();
    prediction_revision_ = buffer_.revision();
    const auto generation = generation_;
    const auto engine_alive = std::weak_ptr<bool>(alive_);
    if (protocol::is_zero(session_id_)) {
        auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
        auto* dispatcher = event_dispatcher_.get();
        service_transport_->open_session([this, context, generation, engine_alive, dispatcher](protocol::Message response) mutable {
            if (engine_alive.expired() || dispatcher == nullptr) return;
            dispatcher->schedule([this, context, generation, engine_alive,
                                  response = std::move(response)]() mutable {
                auto* input_context = context.get();
                if (engine_alive.expired() || input_context == nullptr) return;
                StateScope state_scope(*this, input_context);
                if (generation_ != generation || !prediction_pending_ || !protocol::is_zero(session_id_)) return;
                if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&response)) {
                    session_id_ = opened->session_id;
                    if (auto* state = property(input_context)) {
                        const auto alive = std::weak_ptr<bool>(alive_);
                        const auto session_id = session_id_;
                        state->session_close_handle = [this, alive, session_id]() {
                            if (alive.expired()) return;
                            service_transport_->close_session(session_id, {});
                        };
                    }
                    send_prediction(input_context, generation);
                } else {
                    const bool dirty = prediction_dirty_;
                    prediction_pending_ = false;
                    inflight_request_id_.reset();
                    for (const auto index : prediction_segment_indices_) apply_fallback_candidates(index);
                    prediction_segment_indices_.clear();
                    prediction_dirty_ = false;
                    if (dirty) request_prediction_if_ready(input_context);
                    update_ui(input_context);
                }
            });
        });
    } else {
        send_prediction(input_context, generation);
    }
}

protocol::PredictRequest ImeEngine::build_predict_request(const fcitx::InputContext* input_context) const {
    protocol::PredictRequest request;
    request.session_id = session_id_;
    request.buffer_revision = buffer_.revision();
    for (const auto& segment : buffer_.segments()) {
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

    const auto& surrounding = input_context->surroundingText();
    if (!surrounding.isValid()) return request;

    try {
        auto text = utf8_to_u32(surrounding.text());
        const size_t cursor = std::min<size_t>({surrounding.cursor(), surrounding.anchor(), text.size()});
        text.resize(cursor);

        const size_t reserved_tokens = 2 + request.padding.size() * 2;
        const size_t context_limit = config_.context_length > static_cast<int>(reserved_tokens)
                                          ? static_cast<size_t>(config_.context_length) - reserved_tokens
                                          : 0;
        (void)trim_context_to_token_budget(text, context_limit);

        std::string context_utf8;
        for (const char32_t codepoint : text) context_utf8 += char32_to_utf8(codepoint);
        request.context = utf8_to_u16(context_utf8);
    } catch (const std::runtime_error&) {
        // Ignore malformed surrounding text supplied by a client.
    }
    return request;
}

void ImeEngine::send_prediction(fcitx::InputContext* input_context, std::uint64_t generation) {
    if (generation_ != generation || !prediction_pending_ || protocol::is_zero(session_id_)) return;
    auto request = build_predict_request(input_context);
    request.request_id = next_request_id_++;
    request.buffer_revision = prediction_revision_;
    if (config_.personal_learning_enabled && !config_.base_model_sha256.empty() && !feedback_sensitive_) {
        prediction_context_ = request.context;
        prediction_base_model_hash_ = config_.base_model_sha256;
        prediction_feedback_token_ = {};
    } else {
        prediction_context_.clear();
        prediction_base_model_hash_.clear();
        prediction_feedback_token_ = {};
    }
    inflight_request_id_ = request.request_id;
    inflight_revision_ = request.buffer_revision;
    auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const auto engine_alive = std::weak_ptr<bool>(alive_);
    auto* dispatcher = event_dispatcher_.get();
    service_transport_->predict(
        request.session_id, request.request_id, request.buffer_revision, std::move(request.context),
        std::move(request.padding), [this, context, generation, engine_alive, dispatcher](protocol::Message response) mutable {
            if (engine_alive.expired() || dispatcher == nullptr) return;
            dispatcher->schedule([this, context, generation, engine_alive,
                                  response = std::move(response)]() mutable {
                auto* input_context = context.get();
                if (engine_alive.expired() || input_context == nullptr) return;
                schedule_response(input_context, generation, std::move(response));
            });
        });
}

void ImeEngine::schedule_response(fcitx::InputContext* input_context, std::uint64_t generation,
                                     protocol::Message response) {
    StateScope state_scope(*this, input_context);
    if (generation_ != generation || !prediction_pending_) return;

    bool accepted = false;
    if (const auto* prediction = std::get_if<protocol::Prediction>(&response)) {
        accepted = inflight_request_id_ && *inflight_request_id_ == prediction->request_id &&
                   inflight_revision_ == prediction->buffer_revision &&
                   prediction->session_id == session_id_;
        if (accepted && prediction->candidates.size() == prediction_segment_indices_.size() &&
            prediction_key_ == buffer_.raw_composition() && prediction_revision_ == buffer_.revision()) {
            prediction_feedback_token_ = prediction->feedback_token;
            for (std::size_t i = 0; i < prediction_segment_indices_.size(); ++i) {
                const auto index = prediction_segment_indices_[i];
                if (index < buffer_.segments().size()) {
                    (void)buffer_.set_segment_candidates(index, prediction->candidates[i], true, CandidateSource::Service);
                }
            }
        } else if (accepted) {
            for (const auto index : prediction_segment_indices_) apply_fallback_candidates(index);
        }
    } else if (const auto* error = std::get_if<protocol::Error>(&response)) {
        accepted = !inflight_request_id_ || error->request_id == 0 || error->request_id == *inflight_request_id_;
        if (accepted) {
            if (error->code == protocol::ErrorCode::UnknownSession) session_id_ = {};
            for (const auto index : prediction_segment_indices_) apply_fallback_candidates(index);
        }
    }
    if (!accepted) return;

    const bool dirty = prediction_dirty_;
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
    inflight_revision_ = 0;
    prediction_segment_indices_.clear();
    if (dirty) request_prediction_if_ready(input_context);
    update_ui(input_context);
}

bool ImeEngine::poll_prediction(fcitx::InputContext* input_context) {
    (void)input_context;
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
