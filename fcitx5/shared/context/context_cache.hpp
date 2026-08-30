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
// this IME has committed itself (100% reliable), merges any surrounding text
// the client does provide, and always truncates the oldest part first so the
// model's context window is filled with the text right before the caret.
class ContextCache {
public:
    explicit ContextCache(size_t limit = 1024) : limit_(limit) {}

    // Records text that this IME committed to the application.
    void on_commit(std::u16string text);

    // Resynchronizes against client-supplied surrounding text. `cursor` is
    // the caret offset (in UTF-16 code units) into `text`. The cache keeps
    // its own history when it is longer than the surrounding window; a
    // surrounding window that no longer matches the tail of the cache (user
    // edited the document, moved the caret, switched fields, ...) replaces
    // the cache so it never serves stale context.
    void on_surrounding(std::u16string text, size_t cursor);

    // Removes trailing characters when the user deletes committed text that
    // the client did not reflect back through surrounding text.
    void on_backspace(size_t count = 1);

    // Retention cap in UTF-16 code units. History older than the cap is
    // dropped first. A limit of 0 disables recording entirely.
    void set_limit(size_t limit) noexcept {
        limit_ = limit;
        trim_to_limit();
    }
    size_t limit() const noexcept { return limit_; }

    // True when surrounding text has been seen and was used to resync, or
    // when committed text is being tracked.
    bool valid() const noexcept { return valid_; }

    // The current history, truncated to the last `limit` code units (the
    // oldest text is dropped first).
    std::u16string window(size_t limit) const;

    void clear() noexcept;

private:
    void trim_to_limit() noexcept;

    std::u16string history_;
    size_t limit_;
    bool valid_ = false;
};

}  // namespace ime::fcitx5
