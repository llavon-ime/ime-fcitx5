#pragma once

#include <cstddef>
#include <string>

namespace ime::fcitx5 {

// Samples the text immediately before the caret for prediction context.
//
// AT-SPI exposes the whole text of the focused editable widget (not a
// truncated window), so everything before the caret is authoritative
// regardless of how many lines it spans. The only transformation this helper
// applies is bounding the sample to `max_code_units`, dropping the oldest
// text first (matching the context-cache window semantics used elsewhere).
// An empty widget simply produces no sample. UTF-16 operations never keep a
// half surrogate pair: a pair that does not fit is dropped whole.
class CaretPrefixSampler {
public:
    explicit CaretPrefixSampler(size_t max_code_units) : max_code_units_(max_code_units) {}

    // Replaces the current widget text and caret offset (UTF-16 code units).
    void set_text(std::u16string text, size_t caret) {
        text_before_caret_.clear();
        window_start_ = 0;
        sampled_ = false;
        usable_ = false;
        if (text.empty()) return;

        if (caret > text.size()) caret = text.size();
        text.resize(caret);
        if (!text.empty() && is_high_surrogate(text.back())) text.pop_back();

        sampled_ = true;
        usable_ = true;
        if (text.size() > max_code_units_) {
            size_t start = text.size() - max_code_units_;
            if (is_low_surrogate(text[start]) && start > 0 && is_high_surrogate(text[start - 1])) ++start;
            text.erase(0, start);
            window_start_ = start;
        }
        text_before_caret_ = std::move(text);
    }

    // The text right before the caret after applying the window bound.
    const std::u16string& text_before_caret() const noexcept { return text_before_caret_; }

    // The offset into the original widget text where the retained sample
    // starts (a UTF-16 code unit index, used for debugging / logging).
    std::size_t window_start() const noexcept { return window_start_; }

    // True after at least one sample was attempted.
    bool sampled() const noexcept { return sampled_; }

    // True when the retained text is safe to use as prediction context.
    bool usable() const noexcept { return usable_; }

private:
    static bool is_high_surrogate(char16_t unit) noexcept {
        return unit >= 0xD800 && unit <= 0xDBFF;
    }

    static bool is_low_surrogate(char16_t unit) noexcept {
        return unit >= 0xDC00 && unit <= 0xDFFF;
    }

    std::size_t max_code_units_;
    std::size_t window_start_ = 0;
    std::u16string text_before_caret_;
    bool sampled_ = false;
    bool usable_ = false;
};

}  // namespace ime::fcitx5
