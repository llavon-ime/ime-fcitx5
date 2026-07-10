#include <cstdint>
#include <cstdlib>

#include "input/keypad.hpp"

int run_keypad_tests() {
    bool ok = true;

    ok = ok && !ime::fcitx5::is_keypad_passthrough_keysym('0');
    ok = ok && !ime::fcitx5::is_keypad_passthrough_keysym('9');
    ok = ok && !ime::fcitx5::is_keypad_passthrough_keysym(0xff0d);
    ok = ok && !ime::fcitx5::is_keypad_passthrough_keysym(0xff8d);

    for (std::uint32_t key = 0xffb0; key <= 0xffb9; ++key) {
        ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(key);
    }

    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffaa);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffab);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffac);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffad);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffae);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffaf);
    ok = ok && ime::fcitx5::is_keypad_passthrough_keysym(0xffbd);

    ok = ok && ime::fcitx5::is_return_keysym(0xff0d);
    ok = ok && ime::fcitx5::is_return_keysym(0xff8d);
    ok = ok && !ime::fcitx5::is_return_keysym(0xffb0);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
