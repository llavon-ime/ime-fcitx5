#include "fcitx5/ime_engine.hpp"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
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
#include <string_view>
#include <utility>

#include "bopomofo/keymap.hpp"
#include "input/ascii_tokenizer.hpp"
#include "input/keypad.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

const char* non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

std::string to_utf8(const std::u16string& value) {
    return u16_to_utf8(value);
}

std::string to_utf8(const std::u32string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char32_t codepoint : value) result += char32_to_utf8(codepoint);
    return result;
}

std::u16string to_utf16(char32_t value) {
    return utf8_to_u16(char32_to_utf8(value));
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
#ifdef IME_FCITX5_SOURCE_TABLE_PATH
    const auto source_path = std::filesystem::path(IME_FCITX5_SOURCE_TABLE_PATH);
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
    if (const char* model = non_empty_env("IME_FCITX5_MODEL_PATH")) options.model_path = model;
    return options;
}

char32_t normalize_ascii_letter(char32_t key) {
    if (key >= U'A' && key <= U'Z') return key + (U'a' - U'A');
    return key;
}

bool is_ascii_letter(char16_t ch) {
    return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z');
}

bool is_complete_structured_ascii(std::u16string_view raw) {
    for (const auto& token : tokenize_ascii(raw, 0)) {
        if (token.end != raw.size()) continue;
        switch (token.kind) {
            case AsciiTokenKind::Email:
            case AsciiTokenKind::Domain:
            case AsciiTokenKind::URL:
            case AsciiTokenKind::FilesystemPath:
            case AsciiTokenKind::Identifier:
                return true;
            default:
                break;
        }
    }
    return false;
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
        case FcitxKey_bracketleft:
            return U'{';
        case FcitxKey_bracketright:
            return U'}';
        case FcitxKey_backslash:
            return U'|';
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
        case FcitxKey_grave:
            return U'~';
        default:
            return static_cast<char32_t>(key);
    }
}

// fcitx5 normalizes symbol keys by folding Shift into the keysym and clearing
// the Shift state, so a pressed Shift+comma reaches the engine as sym='<' with
// no Shift bit. A keysym that is itself a shifted symbol therefore counts as
// Shifted.
bool is_shifted_ascii_symbol(char32_t symbol) {
    switch (symbol) {
        case U'!':
        case U'@':
        case U'#':
        case U'$':
        case U'%':
        case U'^':
        case U'&':
        case U'*':
        case U'(':
        case U')':
        case U'_':
        case U'+':
        case U'{':
        case U'}':
        case U'|':
        case U':':
        case U'"':
        case U'<':
        case U'>':
        case U'?':
        case U'~':
            return true;
        default:
            return false;
    }
}

std::optional<char32_t> chewing_punctuation_for_key(const fcitx::Key& key, BopomofoKeyboardLayout layout) {
    if ((key.states() & fcitx::KeyState::Alt) || (key.states() & fcitx::KeyState::Super)) return std::nullopt;
    const char32_t raw_symbol = static_cast<char32_t>(key.sym());
    const char32_t shifted_symbol = shifted_ascii_key(key.sym());
    const bool shifted = static_cast<bool>(key.states() & fcitx::KeyState::Shift) ||
                         is_shifted_ascii_symbol(raw_symbol);
    const char32_t symbol = shifted ? shifted_symbol : raw_symbol;

    if (key.states() & fcitx::KeyState::Ctrl) {
        if (const auto punctuation = lookup_microsoft_ctrl_punctuation_key(symbol)) return punctuation;
        return lookup_microsoft_ctrl_punctuation_key(raw_symbol);
    }

    // On the Hsu layout a punctuation key without Shift commits the halfwidth
    // key symbol itself instead of the fullwidth punctuation.
    if (layout == BopomofoKeyboardLayout::Hsu && !shifted) {
        if (lookup_chewing_punctuation_key(raw_symbol)) return raw_symbol;
        return std::nullopt;
    }

    return lookup_chewing_punctuation_key(symbol);
}

class SelectableCandidateWord final : public fcitx::CandidateWord {
public:
    SelectableCandidateWord(fcitx::Text text, std::function<void(fcitx::InputContext*)> callback)
        : CandidateWord(std::move(text)), callback_(std::move(callback)) {}

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
    input_state_ = state->input_state;
    symbol_menu_ = state->symbol_menu;
    pending_token_ = state->pending_token;
    mixed_decision_ = state->mixed_decision;
    context_cache_ = state->context_cache;
    session_id_ = state->session_id;
    next_request_id_ = state->next_request_id;
    generation_ = state->generation;
    inflight_request_id_ = state->inflight_request_id;
    inflight_revision_ = state->inflight_revision;
    prediction_key_ = state->prediction_key;
    prediction_revision_ = state->prediction_revision;
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
        state->input_state = input_state_;
        state->symbol_menu = symbol_menu_;
        state->pending_token = pending_token_;
        state->mixed_decision = mixed_decision_;
        state->context_cache = context_cache_;
        state->session_id = session_id_;
        state->next_request_id = next_request_id_;
        state->generation = generation_;
        state->inflight_request_id = inflight_request_id_;
        state->inflight_revision = inflight_revision_;
        state->prediction_key = prediction_key_;
        state->prediction_revision = prediction_revision_;
        state->inflight_segment_indices = prediction_segment_indices_;
        state->prediction_pending = prediction_pending_;
        state->prediction_dirty = prediction_dirty_;
    }
    active_input_context_ = nullptr;
}

ImeEngine::ImeEngine(fcitx::Instance* instance)
    : fallback_(default_table_path()),
      decoder_([this](std::u16string_view reading) { return fallback_.lookup(reading); },
               [this](std::u16string_view word) { return fallback_.latin_frequency(word); }),
      service_transport_(default_transport_options()),
      config_(default_config()),
      instance_(instance),
      event_dispatcher_(instance ? &instance->eventDispatcher() : nullptr) {
    if (instance_ != nullptr) {
        (void)instance_->inputContextManager().registerProperty("llavon-ime-input-state", &property_factory_);
    }
    reload_config();
}

ImeEngine::~ImeEngine() {
    if (alive_) *alive_ = false;
}

void ImeEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) {
    if (event.isRelease()) return;

    auto* input_context = event.inputContext();
    StateScope state_scope(*this, input_context);
    const auto raw_key = event.rawKey();
    const auto key = event.key().sym();

    // CapsLock state only exists in the raw key. When Chinese input is
    // disabled under CapsLock, everything passes through to the application
    // (mirrors McBopomofo's capsLockAllowChineseInput=False behavior) and the
    // composition is reset.
    if (raw_key.states() & fcitx::KeyState::CapsLock) {
        if (!config_.caps_lock_inputs_bopomofo) {
            if (!pending_token_.empty()) {
                commit_composition_with(input_context, 0);
            } else {
                buffer_.clear();
                mixed_decision_.clear();
                symbol_menu_.close();
                (void)transition_to(InputState::Empty);
                prediction_pending_ = false;
                prediction_dirty_ = false;
                inflight_request_id_.reset();
                prediction_segment_indices_.clear();
                update_ui(input_context);
            }
            return;
        }
    }

    // Shift+space commits the composition followed by a space; with an empty
    // buffer the key passes through (mirrors McBopomofo's Shift+space).
    if (key == FcitxKey_space && (raw_key.states() & fcitx::KeyState::Shift)) {
        if (composition_empty()) return;
        commit_composition_with(input_context, U' ');
        event.filterAndAccept();
        return;
    }

    const auto layout = config_.keyboard_layout == "hsu" ? BopomofoKeyboardLayout::Hsu
                                                         : BopomofoKeyboardLayout::Standard;
    const auto chewing_punctuation = chewing_punctuation_for_key(event.key(), layout);
    const auto raw_symbol = static_cast<char32_t>(key);
    const bool punctuation_is_standard_bopomofo =
        layout == BopomofoKeyboardLayout::Standard && !has_blocking_modifier(event.key()) &&
        (raw_symbol == U',' || raw_symbol == U'.' || raw_symbol == U';');
    const auto punctuation = punctuation_is_standard_bopomofo ? std::nullopt : chewing_punctuation;
    if (!punctuation && has_blocking_modifier(event.key())) return;

    if (symbol_menu_.active()) {
        handle_symbol_menu_key(input_context, event);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_grave && !has_blocking_modifier(event.key())) {
        if (config_.smart_english && !pending_token_.empty()) {
            (void)settle_pending_preview(input_context);
            open_symbol_menu(input_context);
        } else if (!buffer_.has_unfinished_reading()) {
            open_symbol_menu(input_context);
        }
        event.filterAndAccept();
        return;
    }

    if (is_keypad_passthrough_keysym(static_cast<std::uint32_t>(key))) {
        if (!pending_token_.empty()) (void)settle_pending_preview(input_context);
        if (!buffer_.empty()) commit_current(input_context);
        return;
    }

    if (poll_prediction(input_context)) update_ui(input_context);

    if (candidate_list_active()) {
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
            if (!page_candidates(-1, true)) return;
            update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_Right) {
            if (!page_candidates(1, true)) return;
            update_ui(input_context);
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
            if (mixed_decision_.active()) {
                (void)commit_mixed_candidate(input_context, candidate_cursor_);
            } else {
                (void)select_candidate(input_context, candidate_cursor_);
            }
            event.filterAndAccept();
            return;
        }

        if (key == FcitxKey_space && config_.space_selects_candidate) {
            (void)select_candidate(input_context, candidate_cursor_);
            event.filterAndAccept();
            return;
        }

        const int digit_index = ascii_digit_selection_index(static_cast<std::uint32_t>(key));
        if (digit_index >= 0 && !has_blocking_modifier(event.key())) {
            (void)select_candidate(input_context, candidate_page_offset() + digit_index);
            event.filterAndAccept();
            return;
        }

        if (const auto index = selection_index_for_key(key)) {
            const bool continued_ascii = mixed_decision_.active() && raw_symbol <= 0x7f &&
                                         is_ascii_letter(static_cast<char16_t>(raw_symbol));
            if (!continued_ascii) {
                (void)select_candidate(input_context, candidate_page_offset() + *index);
                event.filterAndAccept();
                return;
            }
        }

        if (chewing_punctuation && !mixed_decision_.active()) {
            event.filterAndAccept();
            return;
        }
    }

    // Smart English keeps the raw input in a pending token; the mixed-input
    // decoder derives every language interpretation. It only resolves at an
    // explicit tone key or Space.
    if (config_.smart_english) {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        const bool caps_on = static_cast<bool>(raw_key.states() & fcitx::KeyState::CapsLock);
        const bool english_intent = (is_upper || is_lower) && (is_upper != caps_on);
        if (english_intent) {
            if (handle_english_letter(input_context, raw_symbol, caps_on)) event.filterAndAccept();
            return;
        }

        const auto smart_layout = pending_token_.empty() ? layout : pending_token_.layout;
        if (!pending_token_.empty()) {
            if (key == FcitxKey_Escape) {
                if (mixed_decision_.active() && input_state_ == InputState::ChoosingCandidate) {
                    (void)transition_to(InputState::Inputting);
                } else if (mixed_decision_.active() && mixed_decision_.preview_path != 0) {
                    mixed_decision_.preview_path = 0;
                    mixed_decision_.preview_character = 0;
                    mixed_decision_.english_boundary = false;
                    mixed_decision_.raw_forced = true;
                } else {
                    pending_token_.clear();
                    mixed_decision_.clear();
                }
                (void)transition_to(composition_empty() ? InputState::Empty : InputState::Inputting);
                update_ui(input_context);
                event.filterAndAccept();
                return;
            }

            if (key == FcitxKey_BackSpace) {
                if (mixed_decision_.active() && input_state_ == InputState::ChoosingCandidate) {
                    (void)transition_to(InputState::Inputting);
                }
                if (mixed_decision_.active() && mixed_decision_.english_boundary) {
                    mixed_decision_.clear();
                    (void)transition_to(InputState::Inputting);
                    update_ui(input_context);
                    event.filterAndAccept();
                    return;
                }
                pending_token_.pop();
                mixed_decision_.clear();
                (void)transition_to(composition_empty() ? InputState::Empty : InputState::Inputting);
                if (pending_token_.empty()) {
                    update_ui(input_context);
                } else {
                    rerun_pending_decision(input_context, false);
                }
                event.filterAndAccept();
                return;
            }

            if (is_return_keysym(static_cast<std::uint32_t>(key))) {
                commit_current(input_context);
                event.filterAndAccept();
                return;
            }

            if (key == FcitxKey_Down && mixed_decision_.active()) {
                (void)show_mixed_candidates(input_context);
                event.filterAndAccept();
                return;
            }

            if (key == FcitxKey_space) {
                if (mixed_decision_.active()) {
                    if (mixed_decision_.preview_path == 0) {
                        if (!mixed_decision_.english_boundary &&
                            is_complete_structured_ascii(pending_token_.raw)) {
                            rerun_pending_decision(input_context, true);
                        } else {
                            (void)commit_composition_with(input_context, U' ');
                        }
                    } else {
                        const auto& preview = mixed_decision_.result.paths[mixed_decision_.preview_path];
                        if (!preview.segments.empty() &&
                            preview.segments.back().kind == MixedSegmentKind::Bopomofo) {
                            (void)settle_pending_preview(input_context);
                        } else {
                            std::u16string latin_tail;
                            for (auto it = preview.segments.rbegin(); it != preview.segments.rend(); ++it) {
                                if (it->kind == MixedSegmentKind::Bopomofo) break;
                                latin_tail.insert(0, it->raw);
                            }
                            const bool long_latin = latin_tail.size() >= 4 &&
                                                    std::all_of(latin_tail.begin(), latin_tail.end(),
                                                                [](char16_t ch) { return is_ascii_letter(ch); });
                            if (fallback_.is_known_english(latin_tail) || long_latin) {
                                (void)commit_composition_with(input_context, U' ');
                            } else {
                                rerun_pending_decision(input_context, true);
                            }
                        }
                    }
                } else {
                    rerun_pending_decision(input_context, true);
                }
                event.filterAndAccept();
                return;
            }

            if (punctuation && (event.key().states() & fcitx::KeyState::Ctrl)) {
                (void)settle_pending_preview(input_context);
                (void)buffer_.add_literal(*punctuation);
                (void)transition_to(InputState::Inputting);
                update_ui(input_context);
                event.filterAndAccept();
                return;
            }

            if (raw_symbol >= 0x21 && raw_symbol <= 0x7e && !has_blocking_modifier(event.key())) {
                if (mixed_decision_.active() && mixed_decision_.english_boundary) {
                    if (mixed_decision_.preview_path == 0) {
                        settle_pending_as_literals();
                        (void)buffer_.add_literal(U' ');
                    } else {
                        (void)settle_pending_preview(input_context);
                    }
                }
                append_pending_char(raw_symbol, smart_layout);
                rerun_pending_decision(input_context, false);
                event.filterAndAccept();
                return;
            }

            // Navigation/editing operates on settled ASCII literals inside the
            // composition rather than committing into the client first.
            (void)settle_pending_preview(input_context);
        }

        const auto& segments = buffer_.segments();
        const bool all_literals = !segments.empty() &&
                                  std::all_of(segments.begin(), segments.end(),
                                              [](const Segment& segment) { return segment.literal != 0; });
        if (pending_token_.empty() && all_literals && !buffer_.caret_at_end() &&
            raw_symbol >= 0x20 && raw_symbol <= 0x7e && !has_blocking_modifier(event.key())) {
            (void)buffer_.add_literal(raw_symbol);
            (void)transition_to(InputState::Inputting);
            mark_prediction_dirty();
            update_ui(input_context);
            event.filterAndAccept();
            return;
        }

        if (pending_token_.empty() && is_smart_start_char(key, layout)) {
            if (candidate_list_active()) (void)transition_to(InputState::Inputting);
            append_pending_char(key, layout);
            rerun_pending_decision(input_context, false);
            event.filterAndAccept();
            return;
        }
    }

    if (punctuation) {
        if (!buffer_.has_unfinished_reading()) {
            (void)buffer_.add_literal(*punctuation);
            (void)transition_to(InputState::Inputting);
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    // Letter keys: the keysym case XOR the CapsLock state decides between
    // English output and bopomofo input, mirroring McBopomofo's case swap.
    // (CapsLock+letter with Chinese disabled already returned above.)
    {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        if (is_upper || is_lower) {
            const bool caps_on = static_cast<bool>(raw_key.states() & fcitx::KeyState::CapsLock);
            if (is_upper != caps_on) {
                if (handle_english_letter(input_context, raw_symbol, caps_on)) event.filterAndAccept();
                return;
            }
        }
    }

    // Match Chewing: digits commit directly from an empty state, join completed
    // composition as literals, and bell while a Hsu syllable is unfinished.
    if (layout == BopomofoKeyboardLayout::Hsu &&
        is_ascii_digit_keysym(static_cast<std::uint32_t>(key))) {
        if (buffer_.has_unfinished_reading()) {
            event.filterAndAccept();
            return;
        }
        if (buffer_.empty()) {
            const std::u16string digit(1, static_cast<char16_t>(key));
            input_context->commitString(to_utf8(digit));
            record_context_commit(digit);
        } else {
            (void)buffer_.add_literal(static_cast<char32_t>(key));
            (void)transition_to(InputState::Inputting);
            update_ui(input_context);
        }
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_space && !buffer_.empty() && !buffer_.has_unfinished_reading_before_caret()) {
        const bool target_complete = current_candidate_target().has_value();
        const bool has_candidates = !available_candidates().empty();
        if (target_complete || has_candidates) {
            if (input_state_ != InputState::ChoosingCandidate) {
                (void)transition_to(InputState::ChoosingCandidate);
                update_ui(input_context);
            } else if (config_.space_selects_candidate && !displayed_candidates_.empty()) {
                (void)select_candidate(input_context, candidate_cursor_);
            }
            event.filterAndAccept();
            return;
        }
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key)) && !composition_empty()) {
        commit_current(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Escape && !composition_empty()) {
        if (handle_escape(input_context)) event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_BackSpace && !buffer_.empty()) {
        buffer_.backspace();
        (void)transition_to(buffer_.empty() ? InputState::Empty : InputState::Inputting);
        mark_prediction_dirty();
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    // Backspace outside the composition is forwarded to the client (which
    // deletes its own text). When enabled, pop the cache so the recorded
    // history does not drift from the document.
    if (key == FcitxKey_BackSpace && composition_empty() && config_.track_context_backspace) {
        context_cache_.on_backspace(1);
    }

    if (key == FcitxKey_Delete && !buffer_.empty()) {
        buffer_.delete_forward();
        (void)transition_to(buffer_.empty() ? InputState::Empty : InputState::Inputting);
        mark_prediction_dirty();
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Left && !buffer_.empty()) {
        if (!buffer_.move_cursor_left()) return;
        (void)transition_to(InputState::Inputting);
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Right && !buffer_.empty()) {
        if (!buffer_.move_cursor_right()) return;
        (void)transition_to(InputState::Inputting);
        update_ui(input_context);
        event.filterAndAccept();
        return;
    }

    if (key == FcitxKey_Down && !buffer_.empty()) {
        const bool target_complete = current_candidate_target().has_value();
        const bool has_candidates = !available_candidates().empty();
        if (target_complete || has_candidates) {
            (void)transition_to(InputState::ChoosingCandidate);
            update_ui(input_context);
            event.filterAndAccept();
            return;
        }
    }

    if (key == FcitxKey_Tab && !buffer_.empty() && !available_candidates().empty()) {
        if (input_state_ != InputState::ChoosingCandidate) {
            (void)transition_to(InputState::ChoosingCandidate);
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

    if (key == FcitxKey_space && buffer_.empty()) return;

    if (const auto input =
            buffer_.add_bopomofo_key(static_cast<char32_t>(key), layout, config_.caps_lock_inputs_bopomofo)) {
        (void)transition_to(InputState::Inputting);
        mark_prediction_dirty();
        if (input->completed) {
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
    StateScope state_scope(*this, event.inputContext());
    reload_config();
    update_ui(event.inputContext());
}

void ImeEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    StateScope state_scope(*this, event.inputContext());
    const bool focus_out = event.type() == fcitx::EventType::InputContextFocusOut;
    const bool complete_composition = !buffer_.empty() && !buffer_.has_unfinished_reading();
    if ((focus_out && (!pending_token_.empty() || complete_composition)) ||
        (!focus_out && event.type() != fcitx::EventType::InputContextReset)) {
        auto text = buffer_.candidate_commit_text();
        text += pending_rendered_text();
        if (!text.empty()) event.inputContext()->commitString(to_utf8(text));
        record_context_commit(text);
    }

    buffer_.clear();
    pending_token_.clear();
    mixed_decision_.clear();
    symbol_menu_.close();
    if (focus_out && config_.reset_context_on_focus_out) context_cache_.clear();
    (void)transition_to(InputState::Empty);
    ++generation_;
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
    prediction_segment_indices_.clear();
    update_ui(event.inputContext());
}

void ImeEngine::reloadConfig() {
    reload_config();
}

void ImeEngine::save() {
    // The default INI location is PkgConfig, matching shared config_path().
    fcitx::safeSaveAsIni(fcitx_config_, kFcitxConfigFile);
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &fcitx_config_;
}

void ImeEngine::setConfig(const fcitx::RawConfig& config) {
    fcitx_config_.load(config, true);
    (void)fcitx_config_.version.setValue(DisplayVersion::Current);
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
            for (const char16_t ch : state->pending_token.raw) {
                (void)state->buffer.add_literal(static_cast<char32_t>(ch));
            }
            state->pending_token.clear();
            state->mixed_decision.clear();
            if (!protocol::is_zero(state->session_id)) {
                service_transport_.close_session(state->session_id, {});
                state->session_id = {};
            }
            state->session_close_handle = {};
            state->invalidate_generation();
            return true;
        });
    }
}

void ImeEngine::reload_config() {
    fcitx_config_ = ImeFcitxConfig();
    try {
        fcitx::readAsIni(fcitx_config_, kFcitxConfigFile);
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
    StateScope state_scope(*this, input_context);
    (void)poll_prediction(input_context);

    if (composition_empty()) {
        if (input_state_ != InputState::Empty) (void)transition_to(InputState::Empty);
        input_context->inputPanel().reset();
        input_context->updatePreedit();
        if (!symbol_menu_.active()) {
            input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            return;
        }
    } else {
        if (input_state_ == InputState::Empty) (void)transition_to(InputState::Inputting);

        auto rendered = buffer_.rendered_composition();
        auto prefix = buffer_.rendered_prefix_before_caret();
        if (!pending_token_.empty()) {
            const auto pending = pending_rendered_text();
            rendered += pending;
            prefix += pending;
        }
        fcitx::Text preedit(to_utf8(rendered));
        preedit.setCursor(static_cast<int>(to_utf8(prefix).size()));
        const bool use_client_preedit = input_context->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
        input_context->inputPanel().setClientPreedit(use_client_preedit ? preedit : fcitx::Text());
        input_context->inputPanel().setPreedit(use_client_preedit ? fcitx::Text() : preedit);
        input_context->inputPanel().setAuxUp(fcitx::Text());
        input_context->inputPanel().setAuxDown(fcitx::Text());
        input_context->updatePreedit();
    }

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    if (mixed_decision_.active() && input_state_ == InputState::ChoosingCandidate) {
        displayed_candidates_.clear();
        const auto entries = decoder_.expand_candidates(
            mixed_decision_.result, candidate_page_size(), mixed_decision_.preview_path);
        for (const auto& entry : entries) displayed_candidates_.push_back(entry.text);
    } else if (symbol_menu_.active()) {
        // Candidates are rendered from symbol_menu_ items below; the placeholder
        // entries keep page and cursor bookkeeping sized identically.
        displayed_candidates_.assign(symbol_menu_.menu().size(), u"?");
    } else {
        displayed_candidates_.clear();
        if (input_state_ == InputState::ChoosingCandidate) {
            for (const char32_t candidate : available_candidates()) {
                displayed_candidates_.push_back(to_utf16(candidate));
            }
        }
    }
    if (displayed_candidates_.empty()) {
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
    const auto target = symbol_menu_.active() ? std::optional<size_t>() : current_candidate_target();
    const auto symbol_epoch = symbol_menu_.epoch();
    int index = 0;
    if (symbol_menu_.active()) {
        for (const auto& item : symbol_menu_.menu()) {
            candidates->append<SelectableCandidateWord>(
                fcitx::Text(to_utf8(item)), [this, index, symbol_epoch](fcitx::InputContext* context) {
                    select_symbol(context, index, symbol_epoch);
                });
            ++index;
        }
    } else {
        for (const auto& candidate : displayed_candidates_) {
            candidates->append<SelectableCandidateWord>(
                fcitx::Text(to_utf8(candidate)),
                [this, index](fcitx::InputContext* context) { select_candidate(context, index); });
            ++index;
        }
    }
    candidates->setPage(candidate_page_);
    if (symbol_menu_.active() || mixed_decision_.active() || target) {
        candidates->setCursorIndex(candidate_cursor_ - candidate_page_offset());
    }
    input_context->inputPanel().setCandidateList(std::move(candidates));
    input_context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ImeEngine::record_context_commit(const std::u16string& text) {
    if (config_.context_history_limit > 0 && !text.empty()) {
        context_cache_.on_commit(text);
    }
}

void ImeEngine::commit_current(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    auto text = buffer_.commit_text();
    text += pending_rendered_text();
    input_context->commitString(to_utf8(text));
    record_context_commit(text);
    buffer_.clear();
    pending_token_.clear();
    mixed_decision_.clear();
    symbol_menu_.close();
    (void)transition_to(InputState::Empty);
    prediction_pending_ = false;
    prediction_dirty_ = false;
    prediction_segment_indices_.clear();
    update_ui(input_context);
}

void ImeEngine::commit_composition_with(fcitx::InputContext* input_context, char32_t extra) {
    StateScope state_scope(*this, input_context);
    std::u16string text = buffer_.commit_text();
    text += pending_rendered_text();
    if (extra != 0) text.push_back(extra);
    buffer_.clear();
    pending_token_.clear();
    mixed_decision_.clear();
    symbol_menu_.close();
    (void)transition_to(InputState::Empty);
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
    prediction_segment_indices_.clear();
    input_context->commitString(to_utf8(text));
    record_context_commit(text);
    update_ui(input_context);
}

bool ImeEngine::handle_english_letter(fcitx::InputContext* input_context, char32_t letter, bool caps_on) {
    StateScope state_scope(*this, input_context);
    if (config_.shift_letter_keys == "directly_put_to_buffer") {
        if (!pending_token_.empty()) (void)settle_pending_preview(input_context);
        const char32_t lower = letter >= U'A' && letter <= U'Z' ? letter + (U'a' - U'A') : letter;
        const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
        if (!buffer_.add_literal(caps_on ? upper : lower)) return false;
        (void)transition_to(InputState::Inputting);
        update_ui(input_context);
        return true;
    }

    // DirectlyOutputUppercase: an empty composition passes the key through,
    // a non-empty composition commits together with the uppercase letter.
    if (composition_empty()) return false;
    const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
    commit_composition_with(input_context, upper);
    return true;
}

bool ImeEngine::is_smart_tone_key(char32_t key, BopomofoKeyboardLayout layout) const {
    if (layout == BopomofoKeyboardLayout::Hsu) {
        return key == U'd' || key == U'f' || key == U'j' || key == U's';
    }
    if (const auto symbol = lookup_bopomofo_key(key)) {
        return is_bopomofo_tone(*symbol) && *symbol != U' ';
    }
    return false;
}

bool ImeEngine::is_smart_start_char(char32_t key, BopomofoKeyboardLayout layout) const {
    if (key >= U'a' && key <= U'z') return true;
    if (layout == BopomofoKeyboardLayout::Standard) {
        if (const auto symbol = lookup_bopomofo_key(key)) return !is_bopomofo_tone(*symbol);
    }
    return false;
}

// Re-decode the exact raw pending keys. A language decision only changes the
// preedit preview; it is not written into the composition until confirmation.
void ImeEngine::rerun_pending_decision(fcitx::InputContext* input_context, bool space_triggered) {
    StateScope state_scope(*this, input_context);
    if (pending_token_.empty()) return;

    std::vector<MixedSegment> previous_chinese;
    if (mixed_decision_.active() && mixed_decision_.preview_path > 0 &&
        mixed_decision_.preview_path < mixed_decision_.result.paths.size() &&
        mixed_decision_.source_revision + (space_triggered ? 0 : 1) == pending_token_.revision) {
        for (const auto& segment : mixed_decision_.result.paths[mixed_decision_.preview_path].segments) {
            if (segment.kind == MixedSegmentKind::Bopomofo) previous_chinese.push_back(segment);
        }
    }

    auto result = decoder_.decode(pending_token_.raw, pending_token_.layout, space_triggered);
    const size_t raw_size = pending_token_.raw.size();
    const size_t no_path = std::numeric_limits<size_t>::max();
    size_t best_closing = no_path;
    size_t best_chinese = no_path;
    size_t best_known_prefix_closing = no_path;
    size_t best_known_tail = no_path;
    size_t best_preserved = no_path;
    size_t best_prefix_len = std::numeric_limits<size_t>::max();
    size_t longest_known_prefix = 0;
    double best_closing_score = -1;
    double best_chinese_score = -1;
    double best_known_prefix_score = -1;
    double best_known_tail_score = -1;
    double best_preserved_score = -1;
    size_t best_chinese_segments = 0;

    for (size_t i = 1; i < result.paths.size(); ++i) {
        const auto& path = result.paths[i];
        if (path.segments.empty()) continue;

        bool has_chinese = false;
        bool seen_chinese = false;
        bool known_latin_tail = false;
        size_t chinese_segments = 0;
        size_t preserved_segments = 0;
        for (const auto& segment : path.segments) {
            if (segment.kind == MixedSegmentKind::Bopomofo) {
                has_chinese = true;
                seen_chinese = true;
                ++chinese_segments;
                if (preserved_segments < previous_chinese.size()) {
                    const auto& previous = previous_chinese[preserved_segments];
                    if (segment.begin == previous.begin && segment.end == previous.end &&
                        segment.reading == previous.reading) {
                        ++preserved_segments;
                    }
                }
            } else if (seen_chinese && segment.kind == MixedSegmentKind::Latin &&
                       fallback_.is_known_english(segment.raw)) {
                known_latin_tail = true;
            }
        }
        if (has_chinese &&
            (chinese_segments > best_chinese_segments ||
             (chinese_segments == best_chinese_segments && path.score > best_chinese_score))) {
            best_chinese = i;
            best_chinese_score = path.score;
            best_chinese_segments = chinese_segments;
        }
        if (known_latin_tail && path.score > best_known_tail_score) {
            best_known_tail = i;
            best_known_tail_score = path.score;
        }
        if (!previous_chinese.empty() && preserved_segments == previous_chinese.size() &&
            path.score > best_preserved_score) {
            best_preserved = i;
            best_preserved_score = path.score;
        }

        if (path.segments.back().kind == MixedSegmentKind::Bopomofo) {
            const size_t begin = path.segments.back().begin;
            if (begin < best_prefix_len ||
                (begin == best_prefix_len && path.score > best_closing_score)) {
                best_closing = i;
                best_prefix_len = begin;
                best_closing_score = path.score;
            }
            if (begin >= 3) {
                const auto prefix = pending_token_.raw.substr(0, begin);
                if (fallback_.is_known_english(prefix) &&
                    (begin > longest_known_prefix ||
                     (begin == longest_known_prefix && path.score > best_known_prefix_score))) {
                    best_known_prefix_closing = i;
                    longest_known_prefix = begin;
                    best_known_prefix_score = path.score;
                }
            }
        }
    }

    const bool explicit_tone = is_smart_tone_key(pending_token_.raw.back(), pending_token_.layout);
    const bool whole_token_closing =
        best_closing != no_path && result.paths[best_closing].segments.back().begin == 0;
    if (space_triggered) {
        const bool all_letters = std::all_of(pending_token_.raw.begin(), pending_token_.raw.end(),
                                             [](char16_t ch) { return is_ascii_letter(ch); });
        if (previous_chinese.empty() &&
            (fallback_.is_known_english(pending_token_.raw) ||
             (all_letters && pending_token_.raw.size() >= 4))) {
            (void)commit_composition_with(input_context, U' ');
            return;
        }
        if (explicit_tone && !whole_token_closing && best_preserved == no_path &&
            best_known_prefix_closing == no_path) {
            (void)commit_composition_with(input_context, U' ');
            return;
        }
        if (best_closing == no_path) {
            (void)commit_composition_with(input_context, U' ');
            return;
        }
        const size_t preview = best_preserved != no_path ? best_preserved :
                               pending_prefers_raw() ? 0 :
                               best_known_prefix_closing != no_path ? best_known_prefix_closing : best_closing;
        set_mixed_preview(input_context, std::move(result), preview, true);
        return;
    }

    bool prefer_raw = pending_prefers_raw();
    if (explicit_tone && raw_size > 4 &&
        fallback_.is_known_english(pending_token_.raw.substr(0, raw_size - 1))) {
        prefer_raw = true;
    }

    size_t preview = no_path;
    if (is_complete_structured_ascii(pending_token_.raw) && best_chinese != no_path) {
        preview = 0;
    } else if (explicit_tone && best_preserved != no_path) {
        preview = best_preserved;
    } else if (best_known_tail != no_path) {
        preview = best_known_tail;
    } else if (explicit_tone && best_known_prefix_closing != no_path) {
        preview = best_known_prefix_closing;
    } else if (prefer_raw && best_chinese != no_path) {
        preview = 0;
    } else if (best_preserved != no_path &&
               pending_token_.layout == BopomofoKeyboardLayout::Standard) {
        preview = best_preserved;
    } else if (explicit_tone && whole_token_closing) {
        preview = best_closing;
    } else if (explicit_tone) {
        preview = 0;
    } else if (pending_token_.layout == BopomofoKeyboardLayout::Standard && best_chinese != no_path) {
        preview = best_chinese;
    }

    if (preview != no_path) {
        set_mixed_preview(input_context, std::move(result), preview, false);
    } else {
        mixed_decision_.clear();
        (void)transition_to(InputState::Inputting);
        update_ui(input_context);
    }
}

void ImeEngine::set_mixed_preview(fcitx::InputContext* input_context, MixedDecodeResult result,
                                  size_t preview_path, bool english_boundary) {
    StateScope state_scope(*this, input_context);
    mixed_decision_.result = std::move(result);
    mixed_decision_.source_revision = pending_token_.revision;
    mixed_decision_.preview_path = preview_path;
    mixed_decision_.preview_character = 0;
    mixed_decision_.english_boundary = english_boundary;
    mixed_decision_.raw_forced = false;
    (void)transition_to(InputState::Inputting);
    update_ui(input_context);
}

bool ImeEngine::show_mixed_candidates(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    if (!mixed_decision_.active() || mixed_decision_.source_revision != pending_token_.revision) return false;

    const auto entries = decoder_.expand_candidates(
        mixed_decision_.result, candidate_page_size(), mixed_decision_.preview_path);
    if (entries.size() < 2) return false;
    (void)transition_to(InputState::ChoosingCandidate);
    candidate_cursor_ = 0;
    for (size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].path_index == mixed_decision_.preview_path &&
            entries[i].char_index == mixed_decision_.preview_character) {
            candidate_cursor_ = static_cast<int>(i);
            break;
        }
    }
    candidate_page_ = candidate_cursor_ / candidate_page_size();
    update_ui(input_context);
    return true;
}

bool ImeEngine::commit_mixed_candidate(fcitx::InputContext* input_context, int index) {
    StateScope state_scope(*this, input_context);
    if (!mixed_decision_.active() || index < 0) return false;
    const auto entries = decoder_.expand_candidates(
        mixed_decision_.result, candidate_page_size(), mixed_decision_.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    auto text = buffer_.commit_text();
    text += entries[static_cast<size_t>(index)].text;
    if (index == 0 && mixed_decision_.english_boundary) text.push_back(u' ');
    input_context->commitString(to_utf8(text));
    record_context_commit(text);
    buffer_.clear();
    pending_token_.clear();
    mixed_decision_.clear();
    symbol_menu_.close();
    (void)transition_to(InputState::Empty);
    prediction_pending_ = false;
    prediction_dirty_ = false;
    inflight_request_id_.reset();
    prediction_segment_indices_.clear();
    update_ui(input_context);
    return true;
}

bool ImeEngine::select_mixed_candidate(fcitx::InputContext* input_context, int index) {
    StateScope state_scope(*this, input_context);
    if (!mixed_decision_.active() || index < 0) return false;
    if (mixed_decision_.source_revision != pending_token_.revision) return false;

    const auto entries = decoder_.expand_candidates(
        mixed_decision_.result, candidate_page_size(), mixed_decision_.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    if (index == 0) {
        if (mixed_decision_.english_boundary) {
            (void)commit_composition_with(input_context, U' ');
        } else {
            mixed_decision_.preview_path = 0;
            mixed_decision_.preview_character = 0;
            mixed_decision_.raw_forced = true;
            (void)transition_to(InputState::Inputting);
            update_ui(input_context);
        }
        return true;
    }

    const auto& entry = entries[static_cast<size_t>(index)];
    if (entry.path_index >= mixed_decision_.result.paths.size()) return false;
    return apply_mixed_path(input_context, mixed_decision_.result.paths[entry.path_index], entry.char_index);
}

bool ImeEngine::apply_mixed_path(fcitx::InputContext* input_context, const MixedPath& path, size_t char_index) {
    StateScope state_scope(*this, input_context);
    CompositionBuffer next = buffer_;
    for (size_t i = 0; i < path.segments.size(); ++i) {
        const auto& segment = path.segments[i];
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            const auto result =
                next.add_bopomofo_keys(segment.body_keys, segment.tone_key, pending_token_.layout, true);
            if (!result || !result->completed) return false;
            (void)next.set_segment_candidates(result->segment_index, segment.candidates);
            const size_t candidate_index = i + 1 == path.segments.size() ? char_index : 0;
            if (candidate_index >= segment.candidates.size()) return false;
            if (!next.select_candidate(result->segment_index, candidate_index,
                                       config_.move_cursor_after_selection)) {
                return false;
            }
        } else {
            for (const char16_t ch : segment.raw) (void)next.add_literal(static_cast<char32_t>(ch));
        }
    }
    buffer_ = std::move(next);
    pending_token_.clear();
    mixed_decision_.clear();
    (void)transition_to(InputState::Inputting);
    mark_prediction_dirty();
    request_prediction_if_ready(input_context);
    update_ui(input_context);
    return true;
}

void ImeEngine::append_pending_char(char32_t key, BopomofoKeyboardLayout layout) {
    pending_token_.push(key, layout);
    (void)transition_to(InputState::Inputting);
    update_ui(active_input_context_);
}

std::u16string ImeEngine::pending_rendered_text() const {
    if (pending_token_.empty()) return {};
    if (!mixed_decision_.active() || mixed_decision_.source_revision != pending_token_.revision ||
        mixed_decision_.result.raw != pending_token_.raw ||
        mixed_decision_.preview_path >= mixed_decision_.result.paths.size()) {
        return pending_token_.raw;
    }
    return mixed_decision_.result.paths[mixed_decision_.preview_path].rendered;
}

void ImeEngine::settle_pending_as_literals() {
    for (const char16_t ch : pending_token_.raw) (void)buffer_.add_literal(static_cast<char32_t>(ch));
    pending_token_.clear();
    mixed_decision_.clear();
    (void)transition_to(buffer_.empty() ? InputState::Empty : InputState::Inputting);
}

bool ImeEngine::settle_pending_preview(fcitx::InputContext* input_context) {
    if (mixed_decision_.active() && mixed_decision_.source_revision == pending_token_.revision &&
        mixed_decision_.preview_path > 0 &&
        mixed_decision_.preview_path < mixed_decision_.result.paths.size()) {
        return apply_mixed_path(input_context,
                                mixed_decision_.result.paths[mixed_decision_.preview_path],
                                mixed_decision_.preview_character);
    }
    settle_pending_as_literals();
    return true;
}

bool ImeEngine::pending_prefers_raw() const {
    if (pending_token_.empty()) return false;
    if (pending_token_.raw.size() == 1 || fallback_.is_known_english(pending_token_.raw)) return true;
    if (is_complete_structured_ascii(pending_token_.raw)) return true;

    const bool all_letters = std::all_of(pending_token_.raw.begin(), pending_token_.raw.end(),
                                         [](char16_t ch) { return is_ascii_letter(ch); });
    if (all_letters && pending_token_.raw.size() >= 4) return true;

    for (const auto& token : tokenize_ascii(pending_token_.raw, 0)) {
        if (token.end == pending_token_.raw.size() && token.kind == AsciiTokenKind::Number) return true;
    }
    return false;
}

bool ImeEngine::select_candidate(fcitx::InputContext* input_context, int index) {
    StateScope state_scope(*this, input_context);
    if (input_state_ != InputState::ChoosingCandidate) return false;
    if (mixed_decision_.active()) return select_mixed_candidate(input_context, index);
    const auto target = current_candidate_target();
    if (!target || index < 0) return false;

    const auto candidates = available_candidates();
    if (index >= static_cast<int>(candidates.size())) return false;

    if (!buffer_.select_candidate(*target, static_cast<size_t>(index), config_.move_cursor_after_selection))
        return false;
    (void)transition_to(InputState::Inputting);
    mark_prediction_dirty();
    update_ui(input_context);
    return true;
}

void ImeEngine::open_symbol_menu(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    symbol_menu_.open();
    if (input_state_ == InputState::ChoosingCandidate) {
        (void)transition_to(InputState::Inputting);
    } else {
        displayed_candidates_.clear();
        reset_candidate_view();
    }
    update_ui(input_context);
}

void ImeEngine::close_symbol_menu(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    symbol_menu_.close();
    displayed_candidates_.clear();
    reset_candidate_view();
    if (buffer_.empty()) {
        if (input_state_ != InputState::Empty) (void)transition_to(InputState::Empty);
    } else {
        (void)transition_to(InputState::Inputting);
    }
    update_ui(input_context);
}

void ImeEngine::handle_symbol_menu_key(fcitx::InputContext* input_context, fcitx::KeyEvent& event) {
    const auto key = event.key().sym();
    if (key == FcitxKey_Escape || key == FcitxKey_grave) {
        close_symbol_menu(input_context);
        return;
    }

    if (key == FcitxKey_BackSpace) {
        if (symbol_menu_.in_category()) {
            symbol_menu_.back();
            reset_candidate_view();
            update_ui(input_context);
        } else {
            close_symbol_menu(input_context);
        }
        return;
    }

    if (key == FcitxKey_Up) {
        if (move_candidate_cursor_in_page(-1)) update_ui(input_context);
        return;
    }
    if (key == FcitxKey_Down) {
        if (move_candidate_cursor_in_page(1)) update_ui(input_context);
        return;
    }
    if (key == FcitxKey_Left || key == FcitxKey_Right) {
        if (page_candidates(key == FcitxKey_Left ? -1 : 1, true)) update_ui(input_context);
        return;
    }
    if (key == FcitxKey_Home || key == FcitxKey_End) {
        const int index = key == FcitxKey_Home ? 0 : static_cast<int>(displayed_candidates_.size()) - 1;
        if (set_candidate_cursor(index)) update_ui(input_context);
        return;
    }
    if (key == FcitxKey_Page_Up || key == FcitxKey_Page_Down) {
        if (page_candidates(key == FcitxKey_Page_Up ? -1 : 1)) {
            (void)set_candidate_cursor(candidate_page_offset());
            update_ui(input_context);
        }
        return;
    }
    if (key == FcitxKey_Tab) {
        candidate_expanded_ = !candidate_expanded_;
        update_ui(input_context);
        return;
    }
    if (is_return_keysym(static_cast<std::uint32_t>(key)) ||
        (key == FcitxKey_space && config_.space_selects_candidate)) {
        (void)select_symbol(input_context, candidate_cursor_, symbol_menu_.epoch());
        return;
    }
    if (const auto index = selection_index_for_key(key)) {
        (void)select_symbol(input_context, candidate_page_offset() + *index, symbol_menu_.epoch());
    }
}

bool ImeEngine::select_symbol(fcitx::InputContext* input_context, int index, std::uint64_t epoch) {
    StateScope state_scope(*this, input_context);
    if (!symbol_menu_.matches(epoch) || index < 0 || index >= static_cast<int>(symbol_menu_.menu().size()))
        return false;

    char32_t symbol = 0;
    if (!symbol_menu_.select(static_cast<size_t>(index), symbol)) {
        // Descended into a category level; keep the menu open.
        reset_candidate_view();
        update_ui(input_context);
        return true;
    }

    symbol_menu_.close();
    displayed_candidates_.clear();
    reset_candidate_view();
    if (!buffer_.add_literal(symbol)) return false;
    (void)transition_to(InputState::Inputting);
    mark_prediction_dirty();
    update_ui(input_context);
    return true;
}

bool ImeEngine::handle_escape(fcitx::InputContext* input_context) {
    StateScope state_scope(*this, input_context);
    const auto manual_target = buffer_.manually_chosen_segment_at_caret();
    const auto action = escape_action(config_.esc_clears_entire_buffer, input_state_,
                                      !available_candidates().empty(),
                                      buffer_.has_unfinished_reading(), manual_target.has_value());
    switch (action) {
        case EscapeAction::ClearBuffer:
            buffer_.clear();
            (void)transition_to(InputState::Empty);
            prediction_pending_ = false;
            prediction_dirty_ = false;
            update_ui(input_context);
            return true;
        case EscapeAction::CloseCandidateList:
            (void)transition_to(InputState::Inputting);
            update_ui(input_context);
            return true;
        case EscapeAction::ClearUnfinishedReading:
            if (!buffer_.clear_unfinished_reading()) return false;
            (void)transition_to(buffer_.empty() ? InputState::Empty : InputState::Inputting);
            mark_prediction_dirty();
            update_ui(input_context);
            return true;
        case EscapeAction::CancelCandidateSelection:
            if (!manual_target || !buffer_.cancel_candidate_selection(*manual_target)) return false;
            (void)transition_to(InputState::Inputting);
            request_prediction_if_ready(input_context);
            update_ui(input_context);
            return true;
        case EscapeAction::KeepBuffer:
            (void)transition_to(InputState::Inputting);
            update_ui(input_context);
            return true;
    }
    return false;
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

bool ImeEngine::transition_to(InputState state) {
    const auto previous = input_state_;
    if (!transition_input_state(input_state_, state)) return false;
    if (state == InputState::ChoosingCandidate) {
        if (previous != InputState::ChoosingCandidate) {
            reset_candidate_view();
            if (const auto target = current_candidate_target()) {
                if (const auto selected = buffer_.segment_selected_index(*target)) {
                    candidate_cursor_ = static_cast<int>(*selected);
                }
            }
        }
    } else {
        displayed_candidates_.clear();
        reset_candidate_view();
    }
    return true;
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

bool ImeEngine::candidate_list_active() const {
    return input_state_ == InputState::ChoosingCandidate && !displayed_candidates_.empty();
}

bool ImeEngine::composition_empty() const {
    return buffer_.empty() && pending_token_.empty();
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
    resync_context_cache(input_context);
    const auto request = build_predict_request(input_context);
    if (request.padding.empty()) return;
    prediction_segment_indices_ = buffer_.completed_segment_indices();
    prediction_pending_ = true;
    prediction_dirty_ = false;
    prediction_key_ = buffer_.raw_composition();
    prediction_revision_ = buffer_.revision();
    const auto generation = generation_;
    const auto engine_alive = std::weak_ptr<bool>(alive_);
    if (protocol::is_zero(session_id_)) {
        auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
        auto* dispatcher = event_dispatcher_;
        service_transport_.open_session([this, context, generation, engine_alive, dispatcher](protocol::Message response) mutable {
            if (engine_alive.expired() || dispatcher == nullptr) return;
            dispatcher->scheduleWithContext(context, [this, context, generation, engine_alive,
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
                            service_transport_.close_session(session_id, {});
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

void ImeEngine::resync_context_cache(const fcitx::InputContext* input_context) {
    if (input_context == nullptr ||
        input_context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return;
    }

    const auto& surrounding = input_context->surroundingText();
    if (!surrounding.isValid()) return;

    try {
        auto text = utf8_to_u32(surrounding.text());
        const size_t cursor = std::min<size_t>({surrounding.cursor(), surrounding.anchor(), text.size()});
        text.resize(cursor);

        std::u16string context_utf16;
        for (const char32_t codepoint : text) context_utf16 += to_utf16(codepoint);
        context_cache_.on_surrounding(std::move(context_utf16), context_utf16.size());
    } catch (const std::runtime_error&) {
        // Ignore malformed surrounding text supplied by a client.
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

    const size_t reserved_tokens = 2 + request.padding.size() * 2;
    const size_t context_limit = config_.context_length > static_cast<int>(reserved_tokens)
                                     ? static_cast<size_t>(config_.context_length) - reserved_tokens
                                     : 0;
    request.context = context_cache_.window(context_limit);
    return request;
}

void ImeEngine::send_prediction(fcitx::InputContext* input_context, std::uint64_t generation) {
    if (generation_ != generation || !prediction_pending_ || protocol::is_zero(session_id_)) return;
    resync_context_cache(input_context);
    auto request = build_predict_request(input_context);
    request.request_id = next_request_id_++;
    request.buffer_revision = prediction_revision_;
    inflight_request_id_ = request.request_id;
    inflight_revision_ = request.buffer_revision;
    auto context = input_context ? input_context->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const auto engine_alive = std::weak_ptr<bool>(alive_);
    auto* dispatcher = event_dispatcher_;
    service_transport_.predict(
        request.session_id, request.request_id, request.buffer_revision, std::move(request.context),
        std::move(request.padding), [this, context, generation, engine_alive, dispatcher](protocol::Message response) mutable {
            if (engine_alive.expired() || dispatcher == nullptr) return;
            dispatcher->scheduleWithContext(context, [this, context, generation, engine_alive,
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
            for (std::size_t i = 0; i < prediction_segment_indices_.size(); ++i) {
                const auto index = prediction_segment_indices_[i];
                if (index < buffer_.segments().size()) {
                    const auto& segment = buffer_.segments()[index];
                    (void)buffer_.set_segment_candidates(
                        index, fallback_.append_alternative_candidates(segment, prediction->candidates[i]));
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

std::vector<char32_t> ImeEngine::available_candidates() const {
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
    if (const auto target = buffer_.candidate_target(candidate_target_mode())) return target;

    // The caret sits at a boundary where the configured side has no segment to
    // select (e.g. position 0 with before-cursor selection). Fall back to the
    // other side so the candidate list can still open and stay visible instead
    // of disappearing (macOS frontend hides the panel when the list is empty).
    const auto fallback = candidate_target_mode() == CandidateTarget::BeforeCursor
                              ? CandidateTarget::AfterCursor
                              : CandidateTarget::BeforeCursor;
    return buffer_.candidate_target(fallback);
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
