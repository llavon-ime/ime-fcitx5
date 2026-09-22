#include "input/keypad.hpp"

namespace llavon::ime {

namespace {

constexpr std::uint32_t kReturn = 0xff0d;
constexpr std::uint32_t kKeypadEnter = 0xff8d;
constexpr std::uint32_t kKeypadMultiply = 0xffaa;
constexpr std::uint32_t kKeypadAdd = 0xffab;
constexpr std::uint32_t kKeypadSeparator = 0xffac;
constexpr std::uint32_t kKeypadSubtract = 0xffad;
constexpr std::uint32_t kKeypadDecimal = 0xffae;
constexpr std::uint32_t kKeypadDivide = 0xffaf;
constexpr std::uint32_t kKeypad0 = 0xffb0;
constexpr std::uint32_t kKeypad9 = 0xffb9;
constexpr std::uint32_t kKeypadEqual = 0xffbd;

}  // namespace

std::optional<char32_t> keypad_operator_keysym(std::uint32_t keysym) {
    switch (keysym) {
        case kKeypadDecimal:
            return U'.';
        case kKeypadSeparator:
            return U',';
        case kKeypadAdd:
            return U'+';
        case kKeypadSubtract:
            return U'-';
        case kKeypadMultiply:
            return U'*';
        case kKeypadDivide:
            return U'/';
        case kKeypadEqual:
            return U'=';
        default:
            return std::nullopt;
    }
}

bool is_ascii_digit_keysym(std::uint32_t keysym) {
    return keysym >= '0' && keysym <= '9';
}

std::optional<char32_t> keypad_digit_keysym(std::uint32_t keysym) {
    if (keysym >= kKeypad0 && keysym <= kKeypad9) {
        return static_cast<char32_t>(U'0' + (keysym - kKeypad0));
    }
    return std::nullopt;
}

int ascii_digit_selection_index(std::uint32_t keysym) {
    if (keysym >= '1' && keysym <= '9') return static_cast<int>(keysym - '1');
    return keysym == '0' ? 9 : -1;
}

bool is_return_keysym(std::uint32_t keysym) {
    return keysym == kReturn || keysym == kKeypadEnter;
}

}  // namespace llavon::ime
