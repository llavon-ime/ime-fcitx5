#include "bopomofo/syllable.hpp"

#include <unordered_set>

namespace ime::fcitx5 {

namespace {

bool contains(const std::unordered_set<char32_t>& set, char32_t symbol) {
    return set.find(symbol) != set.end();
}

void append_bmp(std::u16string& text, char32_t symbol) {
    if (symbol != 0) text.push_back(static_cast<char16_t>(symbol));
}

}  // namespace

bool is_bopomofo_initial(char32_t symbol) {
    static const std::unordered_set<char32_t> values{
        U'ㄅ', U'ㄆ', U'ㄇ', U'ㄈ', U'ㄉ', U'ㄊ', U'ㄋ', U'ㄌ', U'ㄍ', U'ㄎ', U'ㄏ',
        U'ㄐ', U'ㄑ', U'ㄒ', U'ㄓ', U'ㄔ', U'ㄕ', U'ㄖ', U'ㄗ', U'ㄘ', U'ㄙ'};
    return contains(values, symbol);
}

bool is_bopomofo_medial(char32_t symbol) {
    static const std::unordered_set<char32_t> values{U'ㄧ', U'ㄨ', U'ㄩ'};
    return contains(values, symbol);
}

bool is_bopomofo_final(char32_t symbol) {
    static const std::unordered_set<char32_t> values{
        U'ㄚ', U'ㄛ', U'ㄜ', U'ㄝ', U'ㄞ', U'ㄟ', U'ㄠ', U'ㄡ', U'ㄢ', U'ㄣ', U'ㄤ', U'ㄥ', U'ㄦ'};
    return contains(values, symbol);
}

bool is_bopomofo_tone(char32_t symbol) {
    static const std::unordered_set<char32_t> values{U' ', U'ˊ', U'ˇ', U'ˋ', U'˙'};
    return contains(values, symbol);
}

bool Syllable::accept(char32_t symbol) {
    if (complete()) return false;

    if (is_bopomofo_initial(symbol)) {
        if (initial_ != 0 || medial_ != 0 || final_ != 0) return false;
        initial_ = symbol;
        return true;
    }

    if (is_bopomofo_medial(symbol)) {
        if (medial_ != 0 || final_ != 0) return false;
        medial_ = symbol;
        return true;
    }

    if (is_bopomofo_final(symbol)) {
        if (final_ != 0) return false;
        final_ = symbol;
        return true;
    }

    if (is_bopomofo_tone(symbol)) {
        if (tone_ != 0) return false;
        tone_ = symbol;
        return true;
    }

    return false;
}

bool Syllable::overwrite(char32_t symbol) {
    if (is_bopomofo_initial(symbol)) {
        initial_ = symbol;
        return true;
    }

    if (is_bopomofo_medial(symbol)) {
        medial_ = symbol;
        return true;
    }

    if (is_bopomofo_final(symbol)) {
        final_ = symbol;
        return true;
    }

    if (is_bopomofo_tone(symbol)) {
        if (empty()) return false;
        tone_ = symbol;
        return true;
    }

    return false;
}

bool Syllable::pop_back() {
    if (tone_ != 0) {
        tone_ = 0;
        return true;
    }
    if (final_ != 0) {
        final_ = 0;
        return true;
    }
    if (medial_ != 0) {
        medial_ = 0;
        return true;
    }
    if (initial_ != 0) {
        initial_ = 0;
        return true;
    }
    return false;
}

bool Syllable::empty() const noexcept {
    return initial_ == 0 && medial_ == 0 && final_ == 0 && tone_ == 0;
}

bool Syllable::complete() const noexcept {
    return tone_ != 0 && (initial_ != 0 || medial_ != 0 || final_ != 0);
}

std::u16string Syllable::text() const {
    std::u16string result;
    append_bmp(result, initial_);
    append_bmp(result, medial_);
    append_bmp(result, final_);
    append_bmp(result, tone_);
    return result;
}

}  // namespace ime::fcitx5
