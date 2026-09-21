#include <cstdint>
#include <cstdlib>
#include <optional>

#include "input/keypad.hpp"

int run_keypad_tests() {
    bool ok = true;

    ok = ok && !llavon::ime::is_keypad_passthrough_keysym('0');
    ok = ok && !llavon::ime::is_keypad_passthrough_keysym('9');
    ok = ok && !llavon::ime::is_keypad_passthrough_keysym(0xff0d);
    ok = ok && !llavon::ime::is_keypad_passthrough_keysym(0xff8d);

    for (std::uint32_t key = 0xffb0; key <= 0xffb9; ++key) {
        ok = ok && llavon::ime::is_keypad_passthrough_keysym(key);
    }

    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffaa);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffab);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffac);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffad);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffae);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffaf);
    ok = ok && llavon::ime::is_keypad_passthrough_keysym(0xffbd);

    for (std::uint32_t key = '0'; key <= '9'; ++key) {
        ok = ok && llavon::ime::is_ascii_digit_keysym(key);
    }
    ok = ok && !llavon::ime::is_ascii_digit_keysym('/');
    ok = ok && !llavon::ime::is_ascii_digit_keysym(':');
    ok = ok && !llavon::ime::is_ascii_digit_keysym(0xffb0);

    // Keypad digits report the ASCII digit they type; nothing else does.
    ok = ok && llavon::ime::keypad_digit_keysym(0xffb0) == std::optional<char32_t>(U'0');
    ok = ok && llavon::ime::keypad_digit_keysym(0xffb5) == std::optional<char32_t>(U'5');
    ok = ok && llavon::ime::keypad_digit_keysym(0xffb9) == std::optional<char32_t>(U'9');
    ok = ok && !llavon::ime::keypad_digit_keysym(0xffae).has_value();  // KP_Decimal
    ok = ok && !llavon::ime::keypad_digit_keysym(0xffab).has_value();  // KP_Add
    ok = ok && !llavon::ime::keypad_digit_keysym(0xff0d).has_value();  // Return
    ok = ok && !llavon::ime::keypad_digit_keysym(U'5').has_value();

    for (std::uint32_t key = '1'; key <= '9'; ++key) {
        ok = ok && llavon::ime::ascii_digit_selection_index(key) == static_cast<int>(key - '1');
    }
    ok = ok && llavon::ime::ascii_digit_selection_index('0') == 9;
    ok = ok && llavon::ime::ascii_digit_selection_index('/') == -1;
    ok = ok && llavon::ime::ascii_digit_selection_index(0xffb0) == -1;

    ok = ok && llavon::ime::is_return_keysym(0xff0d);
    ok = ok && llavon::ime::is_return_keysym(0xff8d);
    ok = ok && !llavon::ime::is_return_keysym(0xffb0);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
