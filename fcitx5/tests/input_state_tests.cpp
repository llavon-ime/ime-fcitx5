#include <cstdlib>

#include "input/input_state.hpp"

int run_input_state_tests() {
    using ime::fcitx5::InputState;
    using ime::fcitx5::transition_input_state;
    using ime::fcitx5::valid_input_state_transition;

    bool ok = true;
    ok = ok && valid_input_state_transition(InputState::Empty, InputState::Empty);
    ok = ok && valid_input_state_transition(InputState::Empty, InputState::Inputting);
    ok = ok && !valid_input_state_transition(InputState::Empty, InputState::ChoosingCandidate);
    ok = ok && valid_input_state_transition(InputState::Inputting, InputState::Empty);
    ok = ok && valid_input_state_transition(InputState::Inputting, InputState::Inputting);
    ok = ok && valid_input_state_transition(InputState::Inputting, InputState::ChoosingCandidate);
    ok = ok && valid_input_state_transition(InputState::ChoosingCandidate, InputState::Empty);
    ok = ok && valid_input_state_transition(InputState::ChoosingCandidate, InputState::Inputting);
    ok = ok && valid_input_state_transition(InputState::ChoosingCandidate, InputState::ChoosingCandidate);

    auto state = InputState::Empty;
    ok = ok && !transition_input_state(state, InputState::ChoosingCandidate) && state == InputState::Empty;
    ok = ok && transition_input_state(state, InputState::Inputting) && state == InputState::Inputting;
    ok = ok && transition_input_state(state, InputState::ChoosingCandidate) &&
         state == InputState::ChoosingCandidate;
    ok = ok && transition_input_state(state, InputState::Inputting) && state == InputState::Inputting;
    ok = ok && transition_input_state(state, InputState::Empty) && state == InputState::Empty;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
