#pragma once

#include <cstdint>
#include <optional>

namespace llavon::ime {

bool is_keypad_passthrough_keysym(std::uint32_t keysym);
// The ASCII digit a numeric-keypad digit key types (KP_0..KP_9); nullopt for
// every other keysym, including the keypad operators.
std::optional<char32_t> keypad_digit_keysym(std::uint32_t keysym);
bool is_ascii_digit_keysym(std::uint32_t keysym);
int ascii_digit_selection_index(std::uint32_t keysym);
bool is_return_keysym(std::uint32_t keysym);

}  // namespace llavon::ime
