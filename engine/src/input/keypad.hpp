#pragma once

#include <cstdint>
#include <optional>

namespace llavon::ime {

// The ASCII character a numeric-keypad operator key types (KP_Decimal,
// KP_Separator, KP_Add, KP_Subtract, KP_Multiply, KP_Divide, KP_Equal);
// nullopt for every other keysym, including the keypad digits.
std::optional<char32_t> keypad_operator_keysym(std::uint32_t keysym);
// The ASCII digit a numeric-keypad digit key types (KP_0..KP_9); nullopt for
// every other keysym, including the keypad operators.
std::optional<char32_t> keypad_digit_keysym(std::uint32_t keysym);
bool is_ascii_digit_keysym(std::uint32_t keysym);
int ascii_digit_selection_index(std::uint32_t keysym);
bool is_return_keysym(std::uint32_t keysym);

}  // namespace llavon::ime
