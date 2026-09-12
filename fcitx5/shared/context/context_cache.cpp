#include "context/context_cache.hpp"

#include <algorithm>
#include <cstddef>

namespace ime::fcitx5 {

namespace {

bool is_surrogate(char16_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDFFF;
}

bool is_high_surrogate(char16_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool is_low_surrogate(char16_t unit) noexcept {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

void snap_after_split_pair(std::u16string_view text, size_t& start) noexcept {
    if (start < text.size() && is_low_surrogate(text[start]) && start > 0 &&
        is_high_surrogate(text[start - 1])) {
        ++start;
    }
}

size_t scalar_prefix_units(std::u16string_view text) noexcept {
    size_t units = 0;
    while (units < text.size()) {
        if (is_high_surrogate(text[units]) && units + 1 < text.size() && is_low_surrogate(text[units + 1])) {
            units += 2;
        } else if (is_surrogate(text[units])) {
            break;
        } else {
            ++units;
        }
    }
    return units;
}

}  // namespace

void ContextCache::on_commit(std::u16string text) {
    if (text.empty() || limit_ == 0) return;
    history_ += text;
    valid_ = true;
    trim_to_limit();
}

void ContextCache::trim_to_limit() {
    trim_to(limit_);
}

void ContextCache::trim_to(size_t limit) {
    if (limit == 0) {
        clear();
        return;
    }
    if (history_.size() <= limit) return;
    size_t start = history_.size() - limit;
    snap_after_split_pair(history_, start);
    if (start > history_.size()) start = history_.size();
    if (history_.capacity() > limit * 2) {
        std::u16string(history_, start).swap(history_);
    } else {
        history_.erase(0, start);
    }
    if (history_.empty()) valid_ = false;}

void ContextCache::on_surrounding(std::u16string_view text, size_t cursor) {
    cursor = std::min(cursor, text.size());
    text = text.substr(0, cursor);

    const size_t scalar_units = scalar_prefix_units(text);
    text = text.substr(0, scalar_units);
    const size_t limit = limit_ > 0 ? limit_ : surrounding_limit_;
    size_t start = text.size() > limit ? text.size() - limit : 0;
    snap_after_split_pair(text, start);
    text.remove_prefix(start);

    if (text.empty()) {
        clear();
        return;
    }

    // Copy only the retained tail, releasing any larger previous allocation.
    std::u16string(text).swap(history_);
    valid_ = true;
}

void ContextCache::on_backspace(size_t count) {
    if (!valid_ || history_.empty()) return;
    while (count > 0 && !history_.empty()) {
        size_t pop = 1;
        const size_t tail = history_.size() - 1;
        if (tail > 0 && is_low_surrogate(history_[tail]) && is_high_surrogate(history_[tail - 1])) pop = 2;
        history_.resize(history_.size() - pop);
        --count;
    }
    if (history_.empty()) valid_ = false;
}

std::u16string ContextCache::window(size_t limit) const {
    if (history_.empty() || limit == 0) return {};
    if (history_.size() <= limit) return history_;
    size_t start = history_.size() - limit;
    snap_after_split_pair(history_, start);
    if (start > history_.size()) return {};
    return history_.substr(start);
}

void ContextCache::clear() noexcept {
    std::u16string().swap(history_);
    valid_ = false;
}

}  // namespace ime::fcitx5
