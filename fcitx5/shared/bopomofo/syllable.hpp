#pragma once

#include <string>

namespace ime::fcitx5 {

class Syllable {
public:
    bool accept(char32_t symbol);
    bool overwrite(char32_t symbol);
    bool pop_back();
    bool empty() const noexcept;
    bool complete() const noexcept;
    std::u16string text() const;

private:
    char32_t initial_ = 0;
    char32_t medial_ = 0;
    char32_t final_ = 0;
    char32_t tone_ = 0;
};

bool is_bopomofo_initial(char32_t symbol);
bool is_bopomofo_medial(char32_t symbol);
bool is_bopomofo_final(char32_t symbol);
bool is_bopomofo_tone(char32_t symbol);

}  // namespace ime::fcitx5
