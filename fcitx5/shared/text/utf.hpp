#pragma once

#include <string>
#include <string_view>

namespace ime::fcitx5 {

std::u32string utf8_to_u32(std::string_view input);
std::u16string utf8_to_u16(std::string_view input);
// Validates all input, then converts only the tail before a scalar cursor
// (clamped to input length), bounded in UTF-16 units without splitting pairs.
std::u16string utf8_prefix_tail(std::string_view input, size_t cursor, size_t limit);
std::string u16_to_utf8(std::u16string_view input);
std::string char32_to_utf8(char32_t codepoint);
char32_t first_utf8_codepoint(std::string_view input);

}  // namespace ime::fcitx5
