#include "input/keypad.hpp"

namespace ime::fcitx5 {

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

bool is_keypad_passthrough_keysym(std::uint32_t keysym) {
    if (keysym >= kKeypad0 && keysym <= kKeypad9) return true;

    switch (keysym) {
        case kKeypadDecimal:
        case kKeypadSeparator:
        case kKeypadAdd:
        case kKeypadSubtract:
        case kKeypadMultiply:
        case kKeypadDivide:
        case kKeypadEqual:
            return true;
        default:
            return false;
    }
}

bool is_return_keysym(std::uint32_t keysym) {
    return keysym == kReturn || keysym == kKeypadEnter;
}

}  // namespace ime::fcitx5
