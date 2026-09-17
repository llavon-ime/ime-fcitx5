#pragma once

#include <optional>

#include "bopomofo/keymap.hpp"
#include "input/input_key.hpp"

namespace ime::fcitx5 {

// Resolves the punctuation character a key produces under a layout. Returns
// nullopt when the key is not punctuation, is blocked by a modifier, or the
// layout leaves it to the application.
std::optional<char32_t> chewing_punctuation_for_key(const InputKey& key, BopomofoKeyboardLayout layout);

}  // namespace ime::fcitx5
