#include <cstdlib>

#include "input/input_state.hpp"

int run_input_state_tests() {
    using ime::fcitx5::escape_action;
    using ime::fcitx5::EscapeAction;
    using ime::fcitx5::input_state_kind;
    using ime::fcitx5::InputState;
    using ime::fcitx5::InputStateKind;
    using ime::fcitx5::make_input_state;
    using ime::fcitx5::transition_input_state;
    using ime::fcitx5::valid_input_state_transition;

    bool ok = true;
    ok = ok && valid_input_state_transition(InputStateKind::Empty, InputStateKind::Empty);
    ok = ok && valid_input_state_transition(InputStateKind::Empty, InputStateKind::Inputting);
    ok = ok && !valid_input_state_transition(InputStateKind::Empty, InputStateKind::ChoosingCandidate);
    ok = ok && valid_input_state_transition(InputStateKind::Inputting, InputStateKind::Empty);
    ok = ok && valid_input_state_transition(InputStateKind::Inputting, InputStateKind::Inputting);
    ok = ok && valid_input_state_transition(InputStateKind::Inputting, InputStateKind::ChoosingCandidate);
    ok = ok && valid_input_state_transition(InputStateKind::ChoosingCandidate, InputStateKind::Empty);
    ok = ok && valid_input_state_transition(InputStateKind::ChoosingCandidate, InputStateKind::Inputting);
    ok = ok && valid_input_state_transition(InputStateKind::ChoosingCandidate, InputStateKind::ChoosingCandidate);

    InputState state;
    ok = ok && input_state_kind(state) == InputStateKind::Empty;
    ok = ok && input_state_kind(make_input_state(InputStateKind::Empty)) == InputStateKind::Empty;
    ok = ok && input_state_kind(make_input_state(InputStateKind::Inputting)) == InputStateKind::Inputting;
    ok = ok && input_state_kind(make_input_state(InputStateKind::ChoosingCandidate)) ==
                   InputStateKind::ChoosingCandidate;

    ok = ok && !transition_input_state(state, InputStateKind::ChoosingCandidate) &&
         input_state_kind(state) == InputStateKind::Empty;
    ok = ok && transition_input_state(state, InputStateKind::Inputting) &&
         input_state_kind(state) == InputStateKind::Inputting;
    ok = ok && transition_input_state(state, InputStateKind::ChoosingCandidate) &&
         input_state_kind(state) == InputStateKind::ChoosingCandidate;
    ok = ok && transition_input_state(state, InputStateKind::Inputting) &&
         input_state_kind(state) == InputStateKind::Inputting;
    ok = ok && transition_input_state(state, InputStateKind::Empty) &&
         input_state_kind(state) == InputStateKind::Empty;

    // Same-state transitions are accepted and keep the state kind stable.
    ok = ok && transition_input_state(state, InputStateKind::Empty) &&
         input_state_kind(state) == InputStateKind::Empty;

    ok = ok && escape_action(true, InputStateKind::Inputting, false, false, false) == EscapeAction::ClearBuffer;
    ok = ok && escape_action(true, InputStateKind::ChoosingCandidate, true, true, true) == EscapeAction::ClearBuffer;
    ok = ok && escape_action(false, InputStateKind::ChoosingCandidate, true, true, true) ==
                   EscapeAction::CloseCandidateList;
    ok = ok && escape_action(false, InputStateKind::Inputting, false, true, true) ==
                   EscapeAction::ClearUnfinishedReading;
    ok = ok && escape_action(false, InputStateKind::Inputting, true, false, true) ==
                   EscapeAction::CancelCandidateSelection;
    ok = ok && escape_action(false, InputStateKind::Inputting, false, false, false) == EscapeAction::KeepBuffer;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
