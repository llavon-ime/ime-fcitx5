#include "context/context_cache.hpp"

#include <algorithm>

namespace ime::fcitx5 {

void ContextCache::on_commit(std::u16string text) {
    if (text.empty()) return;
    history_ += text;
    valid_ = true;
}

void ContextCache::on_surrounding(std::u16string text, size_t cursor) {
    if (text.empty()) {
        clear();
        return;
    }

    cursor = std::min(cursor, text.size());
    text.resize(cursor);

    // The surrounding window is ground truth. When it no longer ends with
    // our tracked history the document changed behind our back (caret moved,
    // external edit, field switch); follow the client and drop the stale
    // part so we never serve text that is no longer before the caret.
    const size_t overlap = std::min(history_.size(), text.size());
    const bool tail_matches =
        history_.compare(history_.size() - overlap, overlap, text, text.size() - overlap, overlap) == 0;
    if (!tail_matches) {
        history_ = std::move(text);
    } else if (text.size() > history_.size()) {
        // The client window contains older text beyond what we tracked;
        // adopt it so the model gets as much true context as possible.
        history_ = std::move(text);
    }
    valid_ = true;
}

void ContextCache::on_backspace(size_t count) {
    if (!valid_) return;
    if (count >= history_.size()) {
        clear();
        return;
    }
    history_.resize(history_.size() - count);
}

std::u16string ContextCache::window(size_t limit) const {
    if (history_.size() <= limit) return history_;
    return history_.substr(history_.size() - limit);
}

void ContextCache::clear() noexcept {
    history_.clear();
    valid_ = false;
}

}  // namespace ime::fcitx5
