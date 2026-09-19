#pragma once

#include <cstdint>
#include <string>

#include "bopomofo/keymap.hpp"

namespace ime::fcitx5 {

// Pure input facts. All language decisions live in the mixed-input decoder;
// this struct never guesses a language.
struct PendingInput {
    std::u16string raw;
    BopomofoKeyboardLayout layout = BopomofoKeyboardLayout::Standard;
    uint64_t revision = 0;

    bool empty() const noexcept { return raw.empty(); }
    void clear() {
        raw.clear();
        layout = BopomofoKeyboardLayout::Standard;
        ++revision;
    }
    void push(char32_t key, BopomofoKeyboardLayout pending_layout) {
        if (empty()) layout = pending_layout;
        raw.push_back(static_cast<char16_t>(key));
        ++revision;
    }
    void pop() {
        if (raw.empty()) return;
        raw.pop_back();
        ++revision;
    }
};

}  // namespace ime::fcitx5
