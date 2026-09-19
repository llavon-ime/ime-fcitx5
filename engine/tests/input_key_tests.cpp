#include <cstdlib>

#include "input/input_key.hpp"

int run_input_key_tests() {
    using llavon::ime::input_key_state;
    using llavon::ime::InputKey;
    using llavon::ime::InputKeyState;
    using llavon::ime::is_shifted_ascii_symbol;
    using llavon::ime::shifted_ascii_symbol;

    bool ok = true;

    ok = ok && shifted_ascii_symbol(U'1') == U'!';
    ok = ok && shifted_ascii_symbol(U'2') == U'@';
    ok = ok && shifted_ascii_symbol(U'3') == U'#';
    ok = ok && shifted_ascii_symbol(U'4') == U'$';
    ok = ok && shifted_ascii_symbol(U'5') == U'%';
    ok = ok && shifted_ascii_symbol(U'6') == U'^';
    ok = ok && shifted_ascii_symbol(U'7') == U'&';
    ok = ok && shifted_ascii_symbol(U'8') == U'*';
    ok = ok && shifted_ascii_symbol(U'9') == U'(';
    ok = ok && shifted_ascii_symbol(U'0') == U')';
    ok = ok && shifted_ascii_symbol(U'-') == U'_';
    ok = ok && shifted_ascii_symbol(U'=') == U'+';
    ok = ok && shifted_ascii_symbol(U'[') == U'{';
    ok = ok && shifted_ascii_symbol(U']') == U'}';
    ok = ok && shifted_ascii_symbol(U'\\') == U'|';
    ok = ok && shifted_ascii_symbol(U';') == U':';
    ok = ok && shifted_ascii_symbol(U'\'') == U'"';
    ok = ok && shifted_ascii_symbol(U',') == U'<';
    ok = ok && shifted_ascii_symbol(U'.') == U'>';
    ok = ok && shifted_ascii_symbol(U'/') == U'?';
    ok = ok && shifted_ascii_symbol(U'`') == U'~';

    // Letters, digits, and already shifted symbols pass through unchanged.
    ok = ok && shifted_ascii_symbol(U'a') == U'a';
    ok = ok && shifted_ascii_symbol(U'Z') == U'Z';
    ok = ok && shifted_ascii_symbol(U'7') == U'&';
    ok = ok && shifted_ascii_symbol(U'!') == U'!';
    ok = ok && shifted_ascii_symbol(U' ') == U' ';

    ok = ok && is_shifted_ascii_symbol(U'!');
    ok = ok && is_shifted_ascii_symbol(U'~');
    ok = ok && is_shifted_ascii_symbol(U'<');
    ok = ok && !is_shifted_ascii_symbol(U'1');
    ok = ok && !is_shifted_ascii_symbol(U',');
    ok = ok && !is_shifted_ascii_symbol(U'a');
    ok = ok && !is_shifted_ascii_symbol(U' ');

    // A folded shifted symbol counts as shifted even without the state.
    InputKey plain;
    plain.sym = U',';
    ok = ok && !plain.shifted();
    InputKey folded;
    folded.sym = U'<';
    ok = ok && folded.shifted();
    InputKey shifted_state;
    shifted_state.sym = U',';
    shifted_state.states = input_key_state(InputKeyState::Shift);
    ok = ok && shifted_state.shifted();

    // CapsLock and an empty state do not block; every other modifier does.
    InputKey key;
    ok = ok && !key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::CapsLock);
    ok = ok && !key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Shift);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Ctrl);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Alt);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Hyper);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Super);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Super2);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Meta);
    ok = ok && key.has_blocking_modifier();
    key.states = InputKeyState::CapsLock | InputKeyState::Shift | InputKeyState::Ctrl;
    ok = ok && key.has_blocking_modifier() && key.has(InputKeyState::Ctrl) && !key.has(InputKeyState::Alt);

    // The struct defaults describe a released-of-modifiers press.
    InputKey defaults;
    ok = ok && defaults.sym == 0 && defaults.states == 0 && !defaults.caps_lock && !defaults.release;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
