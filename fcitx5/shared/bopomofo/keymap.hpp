#pragma once

#include <optional>
#include <string>
#include <vector>

#include "bopomofo/syllable.hpp"

namespace ime::fcitx5 {

enum class BopomofoKeyboardLayout {
    Standard,
    Hsu,
};

enum class BopomofoKeyStatus {
    Rejected,
    Composing,
    Completed,
};

struct BopomofoKeyResult {
    BopomofoKeyStatus status = BopomofoKeyStatus::Rejected;
    std::vector<std::u16string> alternative_readings;
};

std::optional<char32_t> lookup_bopomofo_key(char32_t key, bool accept_uppercase = true);
std::optional<char32_t> lookup_chewing_punctuation_key(char32_t key);
std::optional<char32_t> lookup_microsoft_ctrl_punctuation_key(char32_t key);

BopomofoKeyResult apply_bopomofo_key(
    Syllable& syllable,
    BopomofoKeyboardLayout layout,
    char32_t key,
    bool accept_uppercase = true);

}  // namespace ime::fcitx5
