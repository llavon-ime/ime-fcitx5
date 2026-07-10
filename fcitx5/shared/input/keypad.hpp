#pragma once

#include <cstdint>

namespace ime::fcitx5 {

bool is_keypad_passthrough_keysym(std::uint32_t keysym);
bool is_return_keysym(std::uint32_t keysym);

}  // namespace ime::fcitx5
