#pragma once

#include <optional>

namespace ime::fcitx5 {

std::optional<char32_t> lookup_bopomofo_key(char32_t key, bool accept_uppercase = true);
std::optional<char32_t> lookup_microsoft_punctuation_key(char32_t key);
std::optional<char32_t> lookup_microsoft_ctrl_punctuation_key(char32_t key);

}  // namespace ime::fcitx5
