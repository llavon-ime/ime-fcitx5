#pragma once

#include <variant>

namespace ime::fcitx5 {

// The editor is idle: no composition and no pending input.
struct EmptyInputState {};

// The user is composing text from bopomofo keys, literal characters, or a
// pending smart-English token.
struct InputtingInputState {};

// The candidate list is open and the user is choosing from it.
struct ChoosingCandidateInputState {};

using InputState = std::variant<EmptyInputState, InputtingInputState, ChoosingCandidateInputState>;

enum class InputStateKind { Empty, Inputting, ChoosingCandidate };

constexpr InputStateKind input_state_kind(const InputState& state) {
    if (std::holds_alternative<ChoosingCandidateInputState>(state)) return InputStateKind::ChoosingCandidate;
    if (std::holds_alternative<InputtingInputState>(state)) return InputStateKind::Inputting;
    return InputStateKind::Empty;
}

constexpr InputState make_input_state(InputStateKind kind) {
    switch (kind) {
        case InputStateKind::Inputting:
            return InputtingInputState{};
        case InputStateKind::ChoosingCandidate:
            return ChoosingCandidateInputState{};
        case InputStateKind::Empty:
            break;
    }
    return EmptyInputState{};
}

constexpr bool valid_input_state_transition(InputStateKind from, InputStateKind to) {
    if (from == to || to == InputStateKind::Empty) return true;
    if (from == InputStateKind::Empty) return to == InputStateKind::Inputting;
    return true;
}

constexpr bool transition_input_state(InputState& current, InputStateKind next) {
    if (!valid_input_state_transition(input_state_kind(current), next)) return false;
    current = make_input_state(next);
    return true;
}

enum class EscapeAction {
    KeepBuffer,
    ClearBuffer,
    CloseCandidateList,
    ClearUnfinishedReading,
    CancelCandidateSelection,
};

constexpr EscapeAction escape_action(bool clear_entire_buffer, InputStateKind state, bool has_candidates,
                                     bool has_unfinished_reading, bool has_manual_choice) {
    if (clear_entire_buffer) return EscapeAction::ClearBuffer;
    if (state == InputStateKind::ChoosingCandidate && has_candidates) return EscapeAction::CloseCandidateList;
    if (has_unfinished_reading) return EscapeAction::ClearUnfinishedReading;
    if (has_manual_choice) return EscapeAction::CancelCandidateSelection;
    return EscapeAction::KeepBuffer;
}

}  // namespace ime::fcitx5
