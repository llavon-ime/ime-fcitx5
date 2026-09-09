#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ime::fcitx5 {

// Self-managed context history for prediction requests.
//
// fcitx5 only exposes surrounding text when the client chooses to push it,
// and even then usually only a short window. This cache accumulates the text
// this IME has committed itself (100% reliable), adopts any surrounding text
// the client does provide. It always truncates the oldest part first so the
// model's context window is filled with the text right before the caret.
// UTF-16 operations preserve complete surrogate pairs.
class ContextCache {
public:
    explicit ContextCache(size_t limit = 1024) : limit_(limit) {}

    // Records text that this IME committed to the application. A limit of 0
    // disables self-managed commit history.
    void on_commit(std::u16string text);

    // Resynchronizes against client-supplied surrounding text. `cursor` is
    // the caret offset (in UTF-16 code units) into `text`. A surrogate pair
    // is never split. Client text is authoritative, so a caret move, external
    // edit, or field switch cannot leave stale context.
    void on_surrounding(std::u16string_view text, size_t cursor);

    // Removes a known number of Unicode scalars. Not suitable for client
    // Backspace events, whose selection/grapheme semantics are unknown.
    void on_backspace(size_t count = 1);

    // Retention cap in UTF-16 code units. History older than the cap is
    // dropped first on scalar boundaries. A limit of 0 disables recording
    // commits, but client-supplied surrounding text remains available.
    void set_limit(size_t limit) {
        if (limit_ != 0 && limit == 0) clear();
        limit_ = limit;
        if (limit_ != 0) trim_to_limit();
    }
    size_t limit() const noexcept { return limit_; }

    // Bounds client-provided surrounding text when self-managed history is
    // disabled. This keeps a large document prefix from being retained just
    // because the client supplied it once.
    void set_surrounding_limit(size_t limit) {
        surrounding_limit_ = limit;
        if (limit_ == 0) trim_to(surrounding_limit_);
    }
    size_t surrounding_limit() const noexcept { return surrounding_limit_; }

    // True when surrounding text has been seen and was used to resync, or
    // when committed text is being tracked.
    bool valid() const noexcept { return valid_; }

    // The current history, truncated to the last `limit` code units (the
    // oldest text is dropped first). It never splits a surrogate pair or
    // exceeds `limit`.
    std::u16string window(size_t limit) const;

    void clear() noexcept;

private:
    void trim_to(size_t limit);
    void trim_to_limit();

    std::u16string history_;
    size_t limit_;
    size_t surrounding_limit_ = 1024;
    bool valid_ = false;
};

}  // namespace ime::fcitx5
