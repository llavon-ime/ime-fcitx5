#pragma once

namespace ime::fcitx5 {

// Candidate selection intent remains active while an asynchronous prediction is pending.
enum class InputState { Empty, Inputting, ChoosingCandidate };

enum class EscapeAction {
    KeepBuffer,
    ClearBuffer,
    CloseCandidateList,
    ClearUnfinishedReading,
    CancelCandidateSelection,
};

constexpr EscapeAction escape_action(bool clear_entire_buffer, InputState state, bool has_candidates,
                                     bool has_unfinished_reading, bool has_manual_choice) {
    if (clear_entire_buffer) return EscapeAction::ClearBuffer;
    if (state == InputState::ChoosingCandidate && has_candidates) return EscapeAction::CloseCandidateList;
    if (has_unfinished_reading) return EscapeAction::ClearUnfinishedReading;
    if (has_manual_choice) return EscapeAction::CancelCandidateSelection;
    return EscapeAction::KeepBuffer;
}

constexpr bool valid_input_state_transition(InputState from, InputState to) {
    if (from == to || to == InputState::Empty) return true;
    if (from == InputState::Empty) return to == InputState::Inputting;
    return true;
}

constexpr bool transition_input_state(InputState& current, InputState next) {
    if (!valid_input_state_transition(current, next)) return false;
    current = next;
    return true;
}

}  // namespace ime::fcitx5
