#pragma once

namespace ime::fcitx5 {

// Candidate selection intent remains active while an asynchronous prediction is pending.
enum class InputState { Empty, Inputting, ChoosingCandidate };

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
