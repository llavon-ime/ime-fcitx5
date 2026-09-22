#include "input/input_processor.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "bopomofo/keymap.hpp"
#include "input/ascii_tokenizer.hpp"
#include "input/keypad.hpp"
#include "input/punctuation.hpp"
#include "text/utf.hpp"

namespace llavon::ime {

namespace {

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

}  // namespace

std::optional<int> selection_index_for_key(char32_t symbol, std::string_view selection_keys,
                                           int selection_key_count, bool caps_lock_inputs_bopomofo) {
    const auto normalized_key = caps_lock_inputs_bopomofo ? ascii_lower(symbol) : symbol;
    const int count = std::min(selection_key_count, static_cast<int>(selection_keys.size()));
    for (int i = 0; i < count; ++i) {
        const auto selection_key =
            static_cast<char32_t>(static_cast<unsigned char>(selection_keys[static_cast<size_t>(i)]));
        if (normalized_key == selection_key) return i;
    }
    return std::nullopt;
}

CandidateKeyOutcome handle_candidate_key(const InputKey& key, const CandidateKeyConfig& config,
                                         CandidateView& view, std::size_t candidate_count,
                                         bool mixed_decision_active, bool has_chewing_punctuation) {
    if (candidate_count == 0) return {};

    switch (key.sym) {
        case keysym::Up:
            return {view.move_cursor(-1, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                            : CandidateKeyAction::Handled,
                    0};
        case keysym::Down:
            return {view.move_cursor(1, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                           : CandidateKeyAction::Handled,
                    0};
        case keysym::Left:
            if (!view.page_by(-1, true, config.page_size, candidate_count)) {
                return {CandidateKeyAction::PassThrough, 0};
            }
            return {CandidateKeyAction::Redraw, 0};
        case keysym::Right:
            if (!view.page_by(1, true, config.page_size, candidate_count)) {
                return {CandidateKeyAction::PassThrough, 0};
            }
            return {CandidateKeyAction::Redraw, 0};
        case keysym::Home:
            return {view.set_cursor(0, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                          : CandidateKeyAction::Handled,
                    0};
        case keysym::End:
            return {view.set_cursor(static_cast<int>(candidate_count) - 1, config.page_size, candidate_count)
                        ? CandidateKeyAction::Redraw
                        : CandidateKeyAction::Handled,
                    0};
        default:
            break;
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym))) {
        return {CandidateKeyAction::ActivateCursor, 0};
    }

    if (key.sym == U' ' && config.space_selects_candidate) {
        return {CandidateKeyAction::SelectIndex, view.cursor};
    }

    const int digit_index = ascii_digit_selection_index(static_cast<std::uint32_t>(key.sym));
    if (digit_index >= 0 && !key.has_blocking_modifier()) {
        const int page_offset = view.page_offset(config.page_size, candidate_count);
        return {CandidateKeyAction::SelectIndex, page_offset + digit_index};
    }

    if (const auto index = selection_index_for_key(key.sym, config.selection_keys, config.selection_key_count,
                                                   config.caps_lock_inputs_bopomofo)) {
        if (!(mixed_decision_active && key.sym <= 0x7f &&
              ((key.sym >= U'a' && key.sym <= U'z') || (key.sym >= U'A' && key.sym <= U'Z')))) {
            const int page_offset = view.page_offset(config.page_size, candidate_count);
            return {CandidateKeyAction::SelectIndex, page_offset + *index};
        }
    }

    if (has_chewing_punctuation && !mixed_decision_active) {
        return {CandidateKeyAction::Handled, 0};
    }

    return {};
}

SymbolMenuKeyOutcome handle_symbol_menu_key(const InputKey& key, const CandidateKeyConfig& config,
                                            SymbolMenuState& menu, CandidateView& view,
                                            std::size_t candidate_count) {
    if (key.sym == keysym::Escape || key.sym == keysym::grave) {
        return {SymbolMenuKeyAction::CloseMenu, 0};
    }

    if (key.sym == keysym::BackSpace) {
        if (menu.in_category()) {
            menu.back();
            view.reset();
            return {SymbolMenuKeyAction::Redraw, 0};
        }
        return {SymbolMenuKeyAction::CloseMenu, 0};
    }

    switch (key.sym) {
        case keysym::Up:
            return {view.move_cursor(-1, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                            : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Down:
            return {view.move_cursor(1, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                           : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Left:
            if (view.page_by(-1, true, config.page_size, candidate_count)) {
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Right:
            if (view.page_by(1, true, config.page_size, candidate_count)) {
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Home:
            return {view.set_cursor(0, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                          : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::End:
            return {view.set_cursor(static_cast<int>(candidate_count) - 1, config.page_size, candidate_count)
                        ? SymbolMenuKeyAction::Redraw
                        : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Page_Up:
        case keysym::Page_Down:
            if (view.page_by(key.sym == keysym::Page_Up ? -1 : 1, false, config.page_size, candidate_count)) {
                (void)view.set_cursor(view.page_offset(config.page_size, candidate_count), config.page_size,
                                      candidate_count);
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Tab:
            view.expanded = !view.expanded;
            return {SymbolMenuKeyAction::Redraw, 0};
        default:
            break;
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym)) ||
        (key.sym == U' ' && config.space_selects_candidate)) {
        return {SymbolMenuKeyAction::ActivateCursor, 0};
    }

    if (const auto index = selection_index_for_key(key.sym, config.selection_keys, config.selection_key_count,
                                                   config.caps_lock_inputs_bopomofo)) {
        return {SymbolMenuKeyAction::SelectIndex,
                view.page_offset(config.page_size, candidate_count) + *index};
    }

    return {};
}

InputProcessor::InputProcessor(FallbackEngine& fallback, MixedInputDecoder& decoder,
                               PhraseOverrideStore& phrase_overrides)
    : fallback_(fallback), decoder_(decoder), phrase_overrides_(phrase_overrides) {}

void InputProcessor::set_state_observer(StateObserver observer) {
    state_observer_ = std::move(observer);
}

void InputProcessor::consume() {
    effect_.handled = true;
}

void InputProcessor::redraw() {
    effect_.redraw = true;
}

void InputProcessor::request_prediction() {
    effect_.request_prediction = true;
}

void InputProcessor::commit_text(std::u16string text) {
    effect_.commit = std::move(text);
}

void InputProcessor::mark_prediction_dirty() {
    session_->prediction.mark_dirty();
}

InputEffect InputProcessor::process(const InputKey& key, InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    process_impl(normalize_key(key));
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::select_candidate(InputSession& session, const Config& config, int index) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    if (select_candidate_impl(index)) consume();
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::select_symbol(InputSession& session, const Config& config, int index,
                                           std::uint64_t epoch) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    if (select_symbol_impl(index, epoch)) consume();
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::reset(InputSession& session, const Config& config, InputResetReason reason,
                                  bool clear_context) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};

    const bool complete_composition = !session.buffer.empty() && !session.buffer.has_unfinished_reading();
    const bool should_commit = reason == InputResetReason::Deactivate ||
                               (reason == InputResetReason::FocusOut &&
                                (!session.pending_token.empty() || complete_composition));
    if (should_commit) {
        auto text = session.buffer.candidate_commit_text();
        text += pending_rendered_text(session);
        commit_text(std::move(text));
    }

    session.buffer.clear();
    session.pending_token.clear();
    session.mixed_decision.clear();
    session.symbol_menu.close();
    if (clear_context) session.context_text.clear();
    (void)transition_to(InputStateKind::Empty);
    session.prediction.invalidate();
    redraw();

    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

void InputProcessor::prepare_for_config_change(InputSession& session) {
    for (const char16_t ch : session.pending_token.raw) {
        (void)session.buffer.add_literal(static_cast<char32_t>(ch));
    }
    session.pending_token.clear();
    session.mixed_decision.clear();
    session.prediction.invalidate();
}

void InputProcessor::process_impl(const InputKey& key) {
    const auto layout = config_->keyboard_layout == "hsu" ? BopomofoKeyboardLayout::Hsu
                                                          : BopomofoKeyboardLayout::Standard;
    const auto raw_symbol = key.sym;

    // CapsLock state only exists in the raw key. When Chinese input is
    // disabled under CapsLock, everything passes through to the application
    // (mirrors McBopomofo's capsLockAllowChineseInput=False behavior) and the
    // composition is reset.
    if (key.caps_lock) {
        if (!config_->caps_lock_inputs_bopomofo) {
            if (!session_->pending_token.empty()) {
                commit_composition_with(0);
            } else {
                session_->buffer.clear();
                session_->mixed_decision.clear();
                session_->symbol_menu.close();
                (void)transition_to(InputStateKind::Empty);
                session_->prediction.clear_pending();
                redraw();
            }
            return;
        }
    }

    // Shift+Left/Right (McBopomofo also accepts Ctrl+Shift) marks a range
    // inside the composition; Enter then stores the marked text as a phrase
    // override. Plain arrows keep moving the caret and clear the mark.
    const bool arrow_key = key.sym == keysym::Left || key.sym == keysym::Right;
    if (arrow_key && key.has(InputKeyState::Shift) &&
        !(key.has(InputKeyState::Alt) || key.has(InputKeyState::Super) || key.has(InputKeyState::Meta)) &&
        !session_->symbol_menu.active() && !candidate_list_active(*session_) && !session_->buffer.empty() &&
        session_->buffer.extend_selection(key.sym == keysym::Left ? -1 : 1)) {
        redraw();
        consume();
        return;
    }

    // Shift+space commits the composition followed by a space; with an empty
    // buffer the key passes through (mirrors McBopomofo's Shift+space).
    if (key.sym == U' ' && key.raw_has(InputKeyState::Shift)) {
        if (composition_empty(*session_)) {
            return;
        }
        commit_composition_with(U' ');
        consume();
        return;
    }

    const auto chewing_punctuation = chewing_punctuation_for_key(key, layout);
    const bool punctuation_is_standard_bopomofo =
        layout == BopomofoKeyboardLayout::Standard && !key.has_blocking_modifier() &&
        (raw_symbol == U',' || raw_symbol == U'.' || raw_symbol == U';');
    const auto punctuation = punctuation_is_standard_bopomofo ? std::nullopt : chewing_punctuation;
    if (!punctuation && key.has_shortcut_modifier()) return;

    if (session_->symbol_menu.active()) {
        const auto outcome = handle_symbol_menu_key(key, candidate_key_config(), session_->symbol_menu,
                                                    session_->candidate_view, session_->symbol_menu.menu().size());
        switch (outcome.action) {
            case SymbolMenuKeyAction::CloseMenu:
                close_symbol_menu();
                break;
            case SymbolMenuKeyAction::ActivateCursor:
                (void)select_symbol_impl(session_->candidate_view.cursor, session_->symbol_menu.epoch());
                break;
            case SymbolMenuKeyAction::SelectIndex:
                (void)select_symbol_impl(outcome.index, session_->symbol_menu.epoch());
                break;
            case SymbolMenuKeyAction::Redraw:
                redraw();
                break;
            case SymbolMenuKeyAction::Handled:
                break;
        }
        consume();
        return;
    }

    if (key.sym == keysym::grave && !key.has_blocking_modifier()) {
        if (config_->smart_english && !session_->pending_token.empty()) {
            (void)settle_pending_preview();
            open_symbol_menu();
        } else if (!session_->buffer.has_unfinished_reading()) {
            open_symbol_menu();
        }
        consume();
        return;
    }

    // The numeric keypad keeps its literal meaning, but a composition in
    // progress absorbs the character it types (a digit, or `.`/`+`/`-`/`*`/
    // `/`/`=`) as a literal instead of being committed: the preedit stays
    // editable and the character commits with it. With nothing to compose the
    // key falls through and is typed by the application.
    auto keypad_literal = keypad_digit_keysym(static_cast<std::uint32_t>(key.sym));
    if (!keypad_literal) keypad_literal = keypad_operator_keysym(static_cast<std::uint32_t>(key.sym));
    if (keypad_literal) {
        if (!session_->pending_token.empty()) (void)settle_pending_preview();
        if (!session_->buffer.empty()) {
            (void)session_->buffer.add_literal(*keypad_literal);
            (void)transition_to(InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        }
    }

    if (session_->choosing_candidate() && candidate_count() != 0) {
        const auto outcome =
            handle_candidate_key(key, candidate_key_config(), session_->candidate_view,
                                 candidate_count(), session_->mixed_decision.active(),
                                 chewing_punctuation.has_value());
        switch (outcome.action) {
            case CandidateKeyAction::PassThrough:
                return;
            case CandidateKeyAction::Handled:
                consume();
                return;
            case CandidateKeyAction::Redraw:
                redraw();
                consume();
                return;
            case CandidateKeyAction::ActivateCursor:
                if (session_->mixed_decision.active()) {
                    (void)commit_mixed_candidate_impl(session_->candidate_view.cursor);
                } else {
                    (void)select_candidate_impl(session_->candidate_view.cursor);
                }
                consume();
                return;
            case CandidateKeyAction::SelectIndex:
                (void)select_candidate_impl(outcome.index);
                consume();
                return;
            case CandidateKeyAction::Unhandled:
                break;
        }
    }

    // Smart English keeps the raw input in a pending token; the mixed-input
    // decoder derives every language interpretation. It only resolves at an
    // explicit tone key or Space.
    if (config_->smart_english) {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        const bool caps_on = key.caps_lock;
        const bool english_intent = (is_upper || is_lower) && (is_upper != caps_on);
        if (english_intent) {
            if (handle_english_letter(raw_symbol, caps_on)) consume();
            return;
        }

        const auto smart_layout = session_->pending_token.empty() ? layout : session_->pending_token.layout;
        if (!session_->pending_token.empty()) {
            if (key.sym == keysym::Escape) {
                (void)session_->buffer.clear_selection();
                if (session_->mixed_decision.active() && session_->choosing_candidate()) {
                    (void)transition_to(InputStateKind::Inputting);
                } else if (session_->mixed_decision.active() && session_->mixed_decision.preview_path != 0) {
                    session_->mixed_decision.preview_path = 0;
                    session_->mixed_decision.preview_character = 0;
                    session_->mixed_decision.english_boundary = false;
                    session_->mixed_decision.raw_forced = true;
                } else {
                    session_->pending_token.clear();
                    session_->mixed_decision.clear();
                }
                (void)transition_to(composition_empty(*session_) ? InputStateKind::Empty : InputStateKind::Inputting);
                redraw();
                consume();
                return;
            }

            if (key.sym == keysym::BackSpace) {
                if (session_->mixed_decision.active() && session_->choosing_candidate()) {
                    (void)transition_to(InputStateKind::Inputting);
                }
                if (session_->mixed_decision.active() && session_->mixed_decision.english_boundary) {
                    session_->mixed_decision.clear();
                    (void)transition_to(InputStateKind::Inputting);
                    redraw();
                    consume();
                    return;
                }
                session_->pending_token.pop();
                session_->mixed_decision.clear();
                (void)transition_to(composition_empty(*session_) ? InputStateKind::Empty : InputStateKind::Inputting);
                if (session_->pending_token.empty()) {
                    redraw();
                } else {
                    rerun_pending_decision(false);
                }
                consume();
                return;
            }

            if (is_return_keysym(static_cast<std::uint32_t>(key.sym))) {
                commit_current();
                consume();
                return;
            }

            if (key.sym == keysym::Down && session_->mixed_decision.active()) {
                (void)show_mixed_candidates();
                consume();
                return;
            }

            if (key.sym == U' ') {
                if (session_->mixed_decision.active()) {
                    if (session_->mixed_decision.preview_path == 0) {
                        if (!session_->mixed_decision.english_boundary &&
                            is_complete_structured_ascii(session_->pending_token.raw)) {
                            rerun_pending_decision(true);
                        } else {
                            (void)commit_composition_with(U' ');
                        }
                    } else {
                        const auto& preview =
                            session_->mixed_decision.result.paths[session_->mixed_decision.preview_path];
                        if (!preview.segments.empty() &&
                            preview.segments.back().kind == MixedSegmentKind::Bopomofo) {
                            (void)settle_pending_preview();
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
                                (void)commit_composition_with(U' ');
                            } else {
                                rerun_pending_decision(true);
                            }
                        }
                    }
                } else {
                    rerun_pending_decision(true);
                }
                consume();
                return;
            }

            if (punctuation && key.has(InputKeyState::Ctrl)) {
                (void)settle_pending_preview();
                (void)session_->buffer.add_literal(*punctuation);
                (void)transition_to(InputStateKind::Inputting);
                redraw();
                consume();
                return;
            }

            if (raw_symbol >= 0x21 && raw_symbol <= 0x7e && !key.has_blocking_modifier()) {
                if (session_->mixed_decision.active() && session_->mixed_decision.english_boundary) {
                    if (session_->mixed_decision.preview_path == 0) {
                        settle_pending_as_literals();
                        (void)session_->buffer.add_literal(U' ');
                    } else {
                        (void)settle_pending_preview();
                    }
                }
                append_pending_char(raw_symbol, smart_layout);
                rerun_pending_decision(false);
                consume();
                return;
            }

            // Navigation/editing operates on settled ASCII literals inside the
            // composition rather than committing into the client first.
            (void)settle_pending_preview();
        }

        const auto& segments = session_->buffer.segments();
        const bool all_literals = !segments.empty() &&
                                  std::all_of(segments.begin(), segments.end(),
                                              [](const Segment& segment) { return segment.literal != 0; });
        if (session_->pending_token.empty() && all_literals && !session_->buffer.caret_at_end() &&
            raw_symbol >= 0x20 && raw_symbol <= 0x7e && !key.has_blocking_modifier()) {
            (void)session_->buffer.add_literal(raw_symbol);
            (void)transition_to(InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        }

        if (session_->pending_token.empty() && is_smart_start_char(key.sym, layout)) {
            if (candidate_list_active(*session_)) (void)transition_to(InputStateKind::Inputting);
            append_pending_char(key.sym, layout);
            rerun_pending_decision(false);
            consume();
            return;
        }
    }

    if (punctuation) {
        if (!session_->buffer.has_unfinished_reading()) {
            (void)session_->buffer.add_literal(*punctuation);
            (void)transition_to(InputStateKind::Inputting);
            redraw();
        }
        consume();
        return;
    }

    // Letter keys: the keysym case XOR the CapsLock state decides between
    // English output and bopomofo input, mirroring McBopomofo's case swap.
    // (CapsLock+letter with Chinese disabled already returned above.)
    {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        if (is_upper || is_lower) {
            const bool caps_on = key.caps_lock;
            if (is_upper != caps_on) {
                if (handle_english_letter(raw_symbol, caps_on)) consume();
                return;
            }
        }
    }

    // Match Chewing: digits commit directly from an empty state, join completed
    // composition as literals, and bell while a Hsu syllable is unfinished.
    if (layout == BopomofoKeyboardLayout::Hsu && is_ascii_digit_keysym(static_cast<std::uint32_t>(key.sym))) {
        if (session_->buffer.has_unfinished_reading()) {
            consume();
            return;
        }
        if (session_->buffer.empty()) {
            commit_text(std::u16string(1, static_cast<char16_t>(key.sym)));
        } else {
            (void)session_->buffer.add_literal(static_cast<char32_t>(key.sym));
            (void)transition_to(InputStateKind::Inputting);
            redraw();
        }
        consume();
        return;
    }

    if (key.sym == U' ' && !session_->buffer.empty() && !session_->buffer.has_unfinished_reading_before_caret()) {
        const bool target_complete = current_candidate_target(*session_, *config_).has_value();
        const bool has_candidates = !available_candidates(*session_, *config_).empty();
        if (target_complete || has_candidates) {
            if (!session_->choosing_candidate()) {
                (void)transition_to(InputStateKind::ChoosingCandidate);
                redraw();
            } else if (config_->space_selects_candidate && candidate_count() != 0) {
                (void)select_candidate_impl(session_->candidate_view.cursor);
            }
            consume();
            return;
        }
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym)) && !composition_empty(*session_)) {
        if (session_->buffer.marked_range()) {
            // Enter in the marking state stores the phrase and keeps composing.
            if (save_marked_phrase_override()) {
                (void)session_->buffer.clear_selection();
                // Pin the freshly stored phrase right away so an immediate
                // commit does not fall back to model output.
                apply_phrase_override(*session_);
                redraw();
            }
            consume();
            return;
        }
        commit_current();
        consume();
        return;
    }

    if (key.sym == keysym::Escape && !composition_empty(*session_)) {
        handle_escape();
        return;
    }

    if (key.sym == keysym::BackSpace && !session_->buffer.empty()) {
        session_->buffer.backspace();
        (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
        mark_prediction_dirty();
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Delete && !session_->buffer.empty()) {
        session_->buffer.delete_forward();
        (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
        mark_prediction_dirty();
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Left && !session_->buffer.empty()) {
        if (!session_->buffer.move_cursor_left()) return;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Right && !session_->buffer.empty()) {
        if (!session_->buffer.move_cursor_right()) return;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Down && !session_->buffer.empty()) {
        const bool target_complete = current_candidate_target(*session_, *config_).has_value();
        const bool has_candidates = !available_candidates(*session_, *config_).empty();
        if (target_complete || has_candidates) {
            (void)transition_to(InputStateKind::ChoosingCandidate);
            redraw();
            consume();
            return;
        }
    }

    if (key.sym == keysym::Tab && !session_->buffer.empty() &&
        !available_candidates(*session_, *config_).empty()) {
        if (!session_->choosing_candidate()) {
            (void)transition_to(InputStateKind::ChoosingCandidate);
        } else {
            session_->candidate_view.expanded = !session_->candidate_view.expanded;
        }
        redraw();
        consume();
        return;
    }

    if ((key.sym == keysym::Page_Up || key.sym == keysym::Page_Down) && !session_->buffer.empty() &&
        candidate_count() != 0) {
        if (page_candidates(key.sym == keysym::Page_Up ? -1 : 1)) {
            (void)set_candidate_cursor(candidate_page_offset(*session_, *config_));
            redraw();
        }
        consume();
        return;
    }

    if (key.sym == U' ' && session_->buffer.empty()) return;

    if (const auto input = session_->buffer.add_bopomofo_key(static_cast<char32_t>(key.sym), layout,
                                                             config_->caps_lock_inputs_bopomofo)) {
        (void)transition_to(InputStateKind::Inputting);
        mark_prediction_dirty();
        if (input->completed) {
            if (const auto segment = session_->buffer.last_edited_segment();
                segment && session_->buffer.segment_complete(*segment)) {
                apply_fallback_candidates(*session_, *segment);
                const auto* candidates = session_->buffer.segment_candidates(*segment);
                if (candidates == nullptr || candidates->empty()) {
                    (void)session_->buffer.remove_segment(*segment);
                    redraw();
                    consume();
                    return;
                }
            }
            request_prediction();
        }
        redraw();
        consume();
        return;
    }
}

bool InputProcessor::transition_to(InputStateKind state) {
    const auto previous = input_state_kind(session_->state);
    if (!transition_input_state(session_->state, state)) return false;
    if (state_observer_) state_observer_(previous, state);
    if (state == InputStateKind::ChoosingCandidate) {
        if (previous != InputStateKind::ChoosingCandidate) {
            reset_candidate_view();
            if (const auto target = current_candidate_target(*session_, *config_)) {
                if (const auto selected = session_->buffer.segment_selected_index(*target)) {
                    session_->candidate_view.cursor = static_cast<int>(*selected);
                }
            }
        }
    } else {
        session_->displayed_candidates.clear();
        reset_candidate_view();
    }
    return true;
}

void InputProcessor::reset_candidate_view() {
    session_->candidate_view.reset();
}

std::size_t InputProcessor::candidate_count() const {
    if (session_->symbol_menu.active()) return session_->symbol_menu.menu().size();
    if (!session_->choosing_candidate()) return 0;
    if (session_->mixed_decision.active()) {
        return decoder_
            .expand_candidates(session_->mixed_decision.result, config_->candidate_page_size,
                               session_->mixed_decision.preview_path)
            .size();
    }
    return available_candidates(*session_, *config_).size();
}

bool InputProcessor::page_candidates(int delta, bool preserve_cursor_offset) {
    return session_->candidate_view.page_by(delta, preserve_cursor_offset, config_->candidate_page_size,
                                             candidate_count());
}

bool InputProcessor::set_candidate_cursor(int index) {
    return session_->candidate_view.set_cursor(index, config_->candidate_page_size,
                                                candidate_count());
}

CandidateKeyConfig InputProcessor::candidate_key_config() const {
    return CandidateKeyConfig{config_->candidate_page_size,  config_->space_selects_candidate,
                              config_->selection_key_count,  config_->caps_lock_inputs_bopomofo,
                              config_->selection_keys};
}

void InputProcessor::commit_current() {
    auto text = session_->buffer.commit_text();
    text += pending_rendered_text(*session_);
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
}

void InputProcessor::commit_composition_with(char32_t extra) {
    std::u16string text = session_->buffer.commit_text();
    text += pending_rendered_text(*session_);
    if (extra != 0) text += utf8_to_u16(char32_to_utf8(extra));
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
}

bool InputProcessor::select_candidate_impl(int index) {
    if (!session_->choosing_candidate()) return false;
    if (session_->mixed_decision.active()) return select_mixed_candidate_impl(index);
    const auto target = current_candidate_target(*session_, *config_);
    if (!target || index < 0) return false;

    const auto candidates = available_candidates(*session_, *config_);
    if (index >= static_cast<int>(candidates.size())) return false;

    if (!session_->buffer.select_candidate(*target, static_cast<size_t>(index),
                                           config_->move_cursor_after_selection)) {
        return false;
    }
    (void)transition_to(InputStateKind::Inputting);
    request_prediction();
    redraw();
    return true;
}

bool InputProcessor::select_symbol_impl(int index, std::uint64_t epoch) {
    if (!session_->symbol_menu.matches(epoch) || index < 0 ||
        index >= static_cast<int>(session_->symbol_menu.menu().size())) {
        return false;
    }

    char32_t symbol = 0;
    if (!session_->symbol_menu.select(static_cast<size_t>(index), symbol)) {
        // Descended into a category level; keep the menu open.
        reset_candidate_view();
        redraw();
        return true;
    }

    session_->symbol_menu.close();
    session_->displayed_candidates.clear();
    reset_candidate_view();
    if (!session_->buffer.add_literal(symbol)) return false;
    (void)transition_to(InputStateKind::Inputting);
    mark_prediction_dirty();
    redraw();
    return true;
}

bool InputProcessor::commit_mixed_candidate_impl(int index) {
    if (!session_->mixed_decision.active() || index < 0) return false;
    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    candidate_page_size(*session_, *config_),
                                                    session_->mixed_decision.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    auto text = session_->buffer.commit_text();
    text += entries[static_cast<size_t>(index)].text;
    if (index == 0 && session_->mixed_decision.english_boundary) text.push_back(u' ');
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
    return true;
}

bool InputProcessor::select_mixed_candidate_impl(int index) {
    if (!session_->mixed_decision.active() || index < 0) return false;
    if (session_->mixed_decision.source_revision != session_->pending_token.revision) return false;

    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    candidate_page_size(*session_, *config_),
                                                    session_->mixed_decision.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    if (index == 0) {
        if (session_->mixed_decision.english_boundary) {
            (void)commit_composition_with(U' ');
        } else {
            session_->mixed_decision.preview_path = 0;
            session_->mixed_decision.preview_character = 0;
            session_->mixed_decision.raw_forced = true;
            (void)transition_to(InputStateKind::Inputting);
            redraw();
        }
        return true;
    }

    const auto& entry = entries[static_cast<size_t>(index)];
    if (entry.path_index >= session_->mixed_decision.result.paths.size()) return false;
    return apply_mixed_path(session_->mixed_decision.result.paths[entry.path_index], entry.char_index);
}

bool InputProcessor::show_mixed_candidates() {
    if (!session_->mixed_decision.active() ||
        session_->mixed_decision.source_revision != session_->pending_token.revision) {
        return false;
    }

    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    candidate_page_size(*session_, *config_),
                                                    session_->mixed_decision.preview_path);
    if (entries.size() < 2) return false;
    (void)transition_to(InputStateKind::ChoosingCandidate);
    session_->candidate_view.cursor = 0;
    for (size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].path_index == session_->mixed_decision.preview_path &&
            entries[i].char_index == session_->mixed_decision.preview_character) {
            session_->candidate_view.cursor = static_cast<int>(i);
            break;
        }
    }
    session_->candidate_view.page = session_->candidate_view.cursor / candidate_page_size(*session_, *config_);
    redraw();
    return true;
}

bool InputProcessor::apply_mixed_path(const MixedPath& path, std::size_t char_index) {
    CompositionBuffer next = session_->buffer;
    for (size_t i = 0; i < path.segments.size(); ++i) {
        const auto& segment = path.segments[i];
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            const auto result =
                next.add_bopomofo_keys(segment.body_keys, segment.tone_key, session_->pending_token.layout, true);
            if (!result || !result->completed) return false;
            (void)next.set_segment_candidates(result->segment_index, segment.candidates);
            const size_t candidate_index = i + 1 == path.segments.size() ? char_index : 0;
            if (candidate_index >= segment.candidates.size()) return false;
            if (!next.select_candidate(result->segment_index, candidate_index,
                                       config_->move_cursor_after_selection)) {
                return false;
            }
        } else {
            for (const char16_t ch : segment.raw) (void)next.add_literal(static_cast<char32_t>(ch));
        }
    }
    session_->buffer = std::move(next);
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    (void)transition_to(InputStateKind::Inputting);
    mark_prediction_dirty();
    request_prediction();
    redraw();
    return true;
}

bool InputProcessor::handle_english_letter(char32_t letter, bool caps_on) {
    if (config_->shift_letter_keys == "directly_put_to_buffer") {
        if (!session_->pending_token.empty()) (void)settle_pending_preview();
        const char32_t lower = ascii_lower(letter);
        const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
        if (!session_->buffer.add_literal(caps_on ? upper : lower)) return false;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        return true;
    }

    // DirectlyOutputUppercase: an empty composition passes the key through,
    // a non-empty composition commits together with the uppercase letter.
    if (composition_empty(*session_)) return false;
    const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
    commit_composition_with(upper);
    return true;
}

void InputProcessor::handle_escape() {
    // Marking only changes the selection, so Escape drops it first without
    // touching the composition itself.
    if (session_->buffer.clear_selection()) {
        redraw();
        consume();
        return;
    }
    const auto manual_target = session_->buffer.manually_chosen_segment_at_caret();
    const auto action = escape_action(config_->esc_clears_entire_buffer, session_->kind(),
                                      !available_candidates(*session_, *config_).empty(),
                                      session_->buffer.has_unfinished_reading(), manual_target.has_value());
    switch (action) {
        case EscapeAction::ClearBuffer:
            session_->buffer.clear();
            (void)transition_to(InputStateKind::Empty);
            session_->prediction.clear_pending();
            redraw();
            consume();
            return;
        case EscapeAction::CloseCandidateList:
            (void)transition_to(InputStateKind::Inputting);
            redraw();
            consume();
            return;
        case EscapeAction::ClearUnfinishedReading:
            if (!session_->buffer.clear_unfinished_reading()) return;
            (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        case EscapeAction::CancelCandidateSelection:
            if (!manual_target || !session_->buffer.cancel_candidate_selection(*manual_target)) return;
            (void)transition_to(InputStateKind::Inputting);
            request_prediction();
            redraw();
            consume();
            return;
        case EscapeAction::KeepBuffer:
            (void)transition_to(InputStateKind::Inputting);
            redraw();
            consume();
            return;
    }
}

void InputProcessor::open_symbol_menu() {
    session_->symbol_menu.open();
    if (session_->choosing_candidate()) {
        (void)transition_to(InputStateKind::Inputting);
    } else {
        session_->displayed_candidates.clear();
        reset_candidate_view();
    }
    redraw();
}

void InputProcessor::close_symbol_menu() {
    session_->symbol_menu.close();
    session_->displayed_candidates.clear();
    reset_candidate_view();
    if (session_->buffer.empty()) {
        if (!session_->empty()) (void)transition_to(InputStateKind::Empty);
    } else {
        (void)transition_to(InputStateKind::Inputting);
    }
    redraw();
}

bool InputProcessor::is_smart_tone_key(char32_t key, BopomofoKeyboardLayout layout) const {
    if (layout == BopomofoKeyboardLayout::Hsu) {
        return key == U'd' || key == U'f' || key == U'j' || key == U's';
    }
    if (const auto symbol = lookup_bopomofo_key(key)) {
        return is_bopomofo_tone(*symbol) && *symbol != U' ';
    }
    return false;
}

bool InputProcessor::is_smart_start_char(char32_t key, BopomofoKeyboardLayout layout) const {
    if (key >= U'a' && key <= U'z') return true;
    if (layout == BopomofoKeyboardLayout::Standard) {
        if (const auto symbol = lookup_bopomofo_key(key)) return !is_bopomofo_tone(*symbol);
    }
    return false;
}

// Re-decode the exact raw pending keys. A language decision only changes the
// preedit preview; it is not written into the composition until confirmation.
void InputProcessor::rerun_pending_decision(bool space_triggered) {
    if (session_->pending_token.empty()) return;

    std::vector<MixedSegment> previous_chinese;
    if (session_->mixed_decision.active() && session_->mixed_decision.preview_path > 0 &&
        session_->mixed_decision.preview_path < session_->mixed_decision.result.paths.size() &&
        session_->mixed_decision.source_revision + (space_triggered ? 0 : 1) == session_->pending_token.revision) {
        for (const auto& segment : session_->mixed_decision.result.paths[session_->mixed_decision.preview_path].segments) {
            if (segment.kind == MixedSegmentKind::Bopomofo) previous_chinese.push_back(segment);
        }
    }

    auto result = decoder_.decode(session_->pending_token.raw, session_->pending_token.layout, space_triggered);
    const size_t raw_size = session_->pending_token.raw.size();
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
                const auto prefix = session_->pending_token.raw.substr(0, begin);
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

    const bool explicit_tone = is_smart_tone_key(session_->pending_token.raw.back(), session_->pending_token.layout);
    const bool whole_token_closing =
        best_closing != no_path && result.paths[best_closing].segments.back().begin == 0;
    if (space_triggered) {
        const bool all_letters = std::all_of(session_->pending_token.raw.begin(), session_->pending_token.raw.end(),
                                             [](char16_t ch) { return is_ascii_letter(ch); });
        if (previous_chinese.empty() &&
            (fallback_.is_known_english(session_->pending_token.raw) ||
             (all_letters && session_->pending_token.raw.size() >= 4))) {
            commit_composition_with(U' ');
            return;
        }
        if (explicit_tone && !whole_token_closing && best_preserved == no_path &&
            best_known_prefix_closing == no_path) {
            commit_composition_with(U' ');
            return;
        }
        if (best_closing == no_path) {
            commit_composition_with(U' ');
            return;
        }
        const size_t preview = best_preserved != no_path ? best_preserved :
                               pending_prefers_raw() ? 0 :
                               best_known_prefix_closing != no_path ? best_known_prefix_closing : best_closing;
        set_mixed_preview(std::move(result), preview, true);
        return;
    }

    bool prefer_raw = pending_prefers_raw();
    if (explicit_tone && raw_size > 4 &&
        fallback_.is_known_english(session_->pending_token.raw.substr(0, raw_size - 1))) {
        prefer_raw = true;
    }

    size_t preview = no_path;
    if (is_complete_structured_ascii(session_->pending_token.raw) && best_chinese != no_path) {
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
               session_->pending_token.layout == BopomofoKeyboardLayout::Standard) {
        preview = best_preserved;
    } else if (explicit_tone && whole_token_closing) {
        preview = best_closing;
    } else if (explicit_tone) {
        preview = 0;
    } else if (session_->pending_token.layout == BopomofoKeyboardLayout::Standard && best_chinese != no_path) {
        preview = best_chinese;
    }

    if (preview != no_path) {
        set_mixed_preview(std::move(result), preview, false);
    } else {
        session_->mixed_decision.clear();
        (void)transition_to(InputStateKind::Inputting);
        redraw();
    }
}

void InputProcessor::set_mixed_preview(MixedDecodeResult result, std::size_t preview_path, bool english_boundary) {
    session_->mixed_decision.result = std::move(result);
    session_->mixed_decision.source_revision = session_->pending_token.revision;
    session_->mixed_decision.preview_path = preview_path;
    session_->mixed_decision.preview_character = 0;
    session_->mixed_decision.english_boundary = english_boundary;
    session_->mixed_decision.raw_forced = false;
    (void)transition_to(InputStateKind::Inputting);
    redraw();
}

void InputProcessor::append_pending_char(char32_t key, BopomofoKeyboardLayout layout) {
    session_->pending_token.push(key, layout);
    (void)transition_to(InputStateKind::Inputting);
    redraw();
}

void InputProcessor::settle_pending_as_literals() {
    for (const char16_t ch : session_->pending_token.raw) (void)session_->buffer.add_literal(static_cast<char32_t>(ch));
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
}

bool InputProcessor::settle_pending_preview() {
    if (session_->mixed_decision.active() &&
        session_->mixed_decision.source_revision == session_->pending_token.revision &&
        session_->mixed_decision.preview_path > 0 &&
        session_->mixed_decision.preview_path < session_->mixed_decision.result.paths.size()) {
        return apply_mixed_path(session_->mixed_decision.result.paths[session_->mixed_decision.preview_path],
                                session_->mixed_decision.preview_character);
    }
    settle_pending_as_literals();
    return true;
}

bool InputProcessor::pending_prefers_raw() const {
    if (session_->pending_token.empty()) return false;
    if (session_->pending_token.raw.size() == 1 || fallback_.is_known_english(session_->pending_token.raw)) return true;
    if (is_complete_structured_ascii(session_->pending_token.raw)) return true;

    const bool all_letters = std::all_of(session_->pending_token.raw.begin(), session_->pending_token.raw.end(),
                                         [](char16_t ch) { return is_ascii_letter(ch); });
    if (all_letters && session_->pending_token.raw.size() >= 4) return true;

    for (const auto& token : tokenize_ascii(session_->pending_token.raw, 0)) {
        if (token.end == session_->pending_token.raw.size() && token.kind == AsciiTokenKind::Number) return true;
    }
    return false;
}

void InputProcessor::apply_fallback_candidates(InputSession& session, std::size_t segment_index) {
    if (!session.buffer.segment_complete(segment_index)) return;

    const auto predictions = fallback_.predict(session.buffer);
    if (segment_index >= predictions.size()) return;
    (void)session.buffer.refresh_segment_candidates(segment_index, predictions[segment_index].candidates);
}

void InputProcessor::apply_prediction(InputSession& session, const protocol::Prediction& prediction) {
    for (std::size_t i = 0; i < session.prediction.segment_indices.size(); ++i) {
        const auto index = session.prediction.segment_indices[i];
        if (index >= session.buffer.segments().size()) continue;
        const auto& segment = session.buffer.segments()[index];
        // Keep the table's homophones in the candidate list so the user can
        // always pick another character, even after a model response.
        (void)session.buffer.refresh_segment_candidates(
            index, fallback_.merge_model_candidates(segment, prediction.candidates[i]));
    }
}

void InputProcessor::reset_state(InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    (void)transition_to(InputStateKind::Empty);
    session_ = nullptr;
    config_ = nullptr;
}

void InputProcessor::sync_state(InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    if (composition_empty(session)) {
        if (!session.empty()) (void)transition_to(InputStateKind::Empty);
    } else if (session.empty()) {
        (void)transition_to(InputStateKind::Inputting);
    }
    session_ = nullptr;
    config_ = nullptr;
}

void InputProcessor::apply_phrase_override(InputSession& session) {
    auto& buffer = session.buffer;
    const auto& segments = buffer.segments();

    // A segment can take part in a stored phrase unless it is unfinished, a
    // literal, or was explicitly chosen by the user.
    const auto usable = [](const Segment& segment) {
        return segment.complete() && segment.literal == 0 && segment.visible_candidate() &&
               !(segment.manually_chosen && !segment.phrase_override_chosen);
    };

    // Existing pins are validated on their own readings: typing more of the
    // composition must not revert the forced text, while editing the pinned
    // range or choosing another candidate inside it releases the pin.
    size_t index = 0;
    while (index < segments.size()) {
        if (!segments[index].phrase_override_chosen) {
            ++index;
            continue;
        }
        size_t end = index;
        std::vector<std::u16string> readings;
        bool valid = true;
        while (end < segments.size() && segments[end].phrase_override_chosen) {
            valid = valid && usable(segments[end]);
            readings.push_back(segments[end].reading());
            ++end;
        }
        if (valid) valid = static_cast<bool>(phrase_overrides_.lookup(readings));
        if (!valid) (void)buffer.clear_phrase_override_choices(index, end - index);
        index = end;
    }

    // Pin stored phrases wherever their readings appear. Leftmost-longest
    // wins so overlapping entries cannot fight over the same readings.
    index = 0;
    while (index < segments.size()) {
        if (!usable(segments[index])) {
            ++index;
            continue;
        }
        size_t usable_length = 0;
        while (index + usable_length < segments.size() && usable(segments[index + usable_length])) {
            ++usable_length;
        }
        if (usable_length < PhraseOverrideStore::kMinReadings) {
            ++index;
            continue;
        }

        size_t matched = 0;
        const size_t longest = std::min(usable_length, PhraseOverrideStore::kMaxReadings);
        for (size_t length = longest; length >= PhraseOverrideStore::kMinReadings; --length) {
            std::vector<std::u16string> readings;
            readings.reserve(length);
            for (size_t i = index; i < index + length; ++i) readings.push_back(segments[i].reading());

            const auto phrase = phrase_overrides_.lookup(readings);
            if (!phrase) continue;

            try {
                const auto codepoints = utf8_to_u32(u16_to_utf8(*phrase));
                if (buffer.apply_phrase_override(index, codepoints)) matched = length;
            } catch (...) {
            }
            break;
        }
        index += matched != 0 ? matched : 1;
    }
}

bool InputProcessor::save_marked_phrase_override() {
    const auto readings = session_->buffer.marked_readings();
    if (!PhraseOverrideStore::valid_entry(session_->buffer.marked_text(), readings.size())) return false;
    return phrase_overrides_.add(session_->buffer.marked_text(), readings);
}

bool InputProcessor::composition_empty(const InputSession& session) {
    return session.buffer.empty() && session.pending_token.empty();
}

bool InputProcessor::candidate_list_active(const InputSession& session) {
    return session.choosing_candidate() && !session.displayed_candidates.empty();
}

int InputProcessor::candidate_page_size(const InputSession& session, const Config& config) {
    return session.candidate_view.page_size(config.candidate_page_size, session.displayed_candidates.size());
}

int InputProcessor::candidate_page_offset(const InputSession& session, const Config& config) {
    return session.candidate_view.page_offset(config.candidate_page_size, session.displayed_candidates.size());
}

void InputProcessor::clamp_candidate_cursor(InputSession& session, const Config& config) {
    session.candidate_view.clamp(config.candidate_page_size, session.displayed_candidates.size());
}

std::vector<char32_t> InputProcessor::available_candidates(const InputSession& session, const Config& config) {
    const auto target = current_candidate_target(session, config);
    if (!target) return {};

    const auto* candidates = session.buffer.segment_candidates(*target);
    if (candidates == nullptr) return {};
    return *candidates;
}

std::optional<std::size_t> InputProcessor::current_candidate_target(const InputSession& session,
                                                                    const Config& config) {
    const auto mode = config.select_phrase == "after_cursor" ? CandidateTarget::AfterCursor
                                                             : CandidateTarget::BeforeCursor;
    if (const auto target = session.buffer.candidate_target(mode)) return target;

    // The caret sits at a boundary where the configured side has no segment to
    // select (e.g. position 0 with before-cursor selection). Fall back to the
    // other side so the candidate list can still open and stay visible instead
    // of disappearing (macOS frontend hides the panel when the list is empty).
    const auto fallback = mode == CandidateTarget::BeforeCursor ? CandidateTarget::AfterCursor
                                                               : CandidateTarget::BeforeCursor;
    return session.buffer.candidate_target(fallback);
}

std::u16string InputProcessor::pending_rendered_text(const InputSession& session) {
    if (session.pending_token.empty()) return {};
    if (!session.mixed_decision.active() ||
        session.mixed_decision.source_revision != session.pending_token.revision ||
        session.mixed_decision.result.raw != session.pending_token.raw ||
        session.mixed_decision.preview_path >= session.mixed_decision.result.paths.size()) {
        return session.pending_token.raw;
    }
    return session.mixed_decision.result.paths[session.mixed_decision.preview_path].rendered;
}

std::u16string InputProcessor::current_preedit(const InputSession& session) {
    auto rendered = session.buffer.rendered_composition();
    const auto pending = pending_rendered_text(session);
    if (!pending.empty()) rendered += pending;
    return rendered;
}

std::u16string InputProcessor::marking_hint_text(const InputSession& session) {
    const auto readings = session.buffer.marked_readings();
    std::u16string hint = u"強制替代詞彙：「" + session.buffer.marked_text() + u"」";
    if (readings.empty()) {
        hint += u"（含未完成的字）— Esc 取消";
    } else if (PhraseOverrideStore::valid_entry(session.buffer.marked_text(), readings.size())) {
        hint += u" — 按 Enter 加入、Esc 取消";
    } else {
        hint += u"（需選取 2 至 8 個字）— Esc 取消";
    }
    return hint;
}

}  // namespace llavon::ime
