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
// An empty widget simply produces no sample.
class CaretPrefixSampler {
public:
    explicit CaretPrefixSampler(size_t max_code_units) : max_code_units_(max_code_units) {}

    // Replaces the current widget text and caret offset (UTF-16 code units).
    void set_text(std::u16string text, size_t caret) {
        text_ = std::move(text);
        sampled_ = false;
        usable_ = false;
        window_start_ = 0;
        text_before_caret_.clear();
        if (text_.empty()) return;

        if (caret > text_.size()) caret = text_.size();
        caret_ = caret;
        sampled_ = true;
        usable_ = true;
        emit_prefix();
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
    void emit_prefix() {
        std::u16string prefix = text_.substr(0, caret_);
        if (prefix.size() > max_code_units_) {
            const size_t drop = prefix.size() - max_code_units_;
            prefix.erase(0, drop);
            window_start_ += drop;
        }
        text_before_caret_ = std::move(prefix);
    }

    std::size_t max_code_units_;
    std::u16string text_;
    std::size_t caret_ = 0;
    std::size_t window_start_ = 0;
    std::u16string text_before_caret_;
    bool sampled_ = false;
    bool usable_ = false;
};

}  // namespace ime::fcitx5
