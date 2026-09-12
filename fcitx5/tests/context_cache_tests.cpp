#include "context/context_cache.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace ime::fcitx5 {
namespace {

std::u16string utf16(const char* text) {
    return utf8_to_u16(text);
}

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

bool test_commit_accumulates() {
    ContextCache cache;
    cache.on_commit(utf16("你好"));
    cache.on_commit(utf16("，world"));
    return check(cache.window(100) == utf16("你好，world"), "commit accumulates in order");
}

bool test_window_truncates_oldest() {
    ContextCache cache;
    cache.on_commit(utf16("abcdefghij"));
    bool ok = check(cache.window(4) == utf16("ghij"), "window keeps the newest 4 code units");
    ok &= check(cache.window(100) == utf16("abcdefghij"), "window below limit keeps everything");
    ok &= check(cache.window(0).empty(), "zero limit yields empty window");
    return ok;
}

bool test_commit_without_previous_history() {
    ContextCache cache;
    cache.on_commit(utf16("abc"));
    bool ok = check(cache.valid(), "commit marks cache valid");
    ok &= check(cache.window(10) == utf16("abc"), "committed text is returned");
    return ok;
}

bool test_surrounding_tail_extends_cache() {
    // Client provides a window that ends with what we committed; the cache
    // adopts the surrounding text as ground truth (it may contain older text
    // the client kept around).
    ContextCache cache;
    cache.on_commit(utf16("world"));
    cache.on_surrounding(utf16("hello world"), 11);
    bool ok = check(cache.window(100) == utf16("hello world"), "surrounding extends the cache");
    return ok;
}

bool test_surrounding_mismatch_replaces_cache() {
    // The client moved the caret / edited elsewhere; surrounding no longer
    // matches our tail. The cache must follow the client, not keep stale text.
    ContextCache cache;
    cache.on_commit(utf16("world"));
    cache.on_surrounding(utf16("something else entirely"), 8);
    bool ok = check(cache.window(100) == utf16("somethin"), "mismatch replaces the cache with surrounding text");
    return ok;
}

bool test_surrounding_shorter_replaces_history() {
    // Without an acknowledged base, a shorter client snapshot may represent
    // a caret move and must be treated as authoritative.
    ContextCache cache;
    cache.on_commit(utf16("你好world"));
    cache.on_surrounding(utf16("world"), 5);
    bool ok = check(cache.window(100) == utf16("world"), "ambiguous short surrounding replaces history");
    return ok;
}

bool test_surrounding_prefix_replaces_cache() {
    ContextCache cache;
    cache.on_surrounding(utf16("hello"), 5);
    cache.on_commit(utf16(" world"));
    cache.on_surrounding(utf16("hello"), 5);
    return check(cache.window(100) == utf16("hello"),
                 "surrounding prefix drops text that may be after the caret");
}

bool test_surrounding_scalar_safe() {
    ContextCache cache;
    std::u16string prefix(1, static_cast<char16_t>(0xD83D));
    cache.on_surrounding(prefix, 1);
    return check(cache.window(10).empty(), "a lone high surrogate is not kept");
}

bool test_surrounding_empty_resets() {
    ContextCache cache;
    cache.on_commit(utf16("abc"));
    cache.on_surrounding(std::u16string(), 0);
    return check(!cache.valid() && cache.window(10).empty(), "empty surrounding resets the cache");
}

bool test_backspace_pops_tail() {
    ContextCache cache;
    cache.on_commit(utf16("hello"));
    cache.on_backspace(2);
    return check(cache.window(100) == utf16("hel"), "backspace removes the trailing code units");
}

bool test_backspace_beyond_length_clears() {
    ContextCache cache;
    cache.on_commit(utf16("ab"));
    cache.on_backspace(10);
    return check(cache.window(100).empty(), "backspace beyond length clears the cache");
}

bool test_backspace_pops_whole_scalar() {
    ContextCache cache;
    std::u16string emoji;
    emoji.push_back(static_cast<char16_t>(0xD83D));
    emoji.push_back(static_cast<char16_t>(0xDE00));
    cache.on_commit(emoji);
    cache.on_backspace();
    return check(cache.window(100).empty(), "backspace removes a complete surrogate pair");
}

bool test_clear() {
    ContextCache cache;
    cache.on_commit(utf16("abc"));
    cache.clear();
    return check(!cache.valid() && cache.window(10).empty(), "clear resets validity and history");
}

bool test_utf16_surrogate_handling() {
    // A pair that cannot fit is dropped whole, never split or returned over
    // the requested code-unit budget.
    ContextCache cache;
    std::u16string emoji;
    emoji.push_back(static_cast<char16_t>(0xD83D));
    emoji.push_back(static_cast<char16_t>(0xDE00));
    cache.on_commit(emoji);
    bool ok = check(cache.window(1).empty(), "an unfittable surrogate pair is dropped whole");
    ok &= check(cache.window(2) == emoji, "a fitting surrogate pair survives");
    return ok;
}

bool test_retention_cap_keeps_pairs_whole() {
    ContextCache cache(3);
    std::u16string emoji;
    emoji.push_back(static_cast<char16_t>(0xD83D));
    emoji.push_back(static_cast<char16_t>(0xDE00));
    cache.on_commit(emoji);
    cache.on_commit(utf16("ab"));
    return check(cache.window(100) == utf16("ab"),
                 "a surrogate pair straddling the retention cap is dropped whole");
}

bool test_retention_cap() {
    ContextCache cache(4);
    cache.on_commit(utf16("abcd"));
    bool ok = check(cache.window(100) == utf16("abcd"), "history up to the cap is kept whole");
    cache.on_commit(utf16("ef"));
    ok &= check(cache.window(100) == utf16("cdef"), "commits beyond the cap drop the oldest first");
    ok &= check(cache.window(2) == utf16("ef"), "window() still trims independently of the cap");
    return ok;
}

bool test_retention_cap_zero_disables() {
    ContextCache cache(0);
    cache.on_commit(utf16("abc"));
    return check(!cache.valid() && cache.window(10).empty(), "zero cap records nothing");
}

bool test_retention_cap_resync_respects() {
    // Surrounding text adoption is also bounded by the cap.
    ContextCache cache(4);
    cache.on_commit(utf16("x"));
    cache.on_surrounding(utf16("hello world"), 5);
    return check(cache.window(100) == utf16("ello"), "resynced history is capped too");
}

bool test_retention_cap_set_limit() {
    ContextCache cache;
    cache.on_commit(utf16("abcdef"));
    cache.set_limit(3);
    bool ok = check(cache.window(100) == utf16("def"), "set_limit trims existing history");
    cache.set_limit(10);
    cache.on_commit(utf16("ghi"));
    ok &= check(cache.window(100) == utf16("defghi"), "growing the limit allows more history");
    return ok;
}

bool test_surrounding_with_zero_limit_is_kept() {
    ContextCache cache(0);
    cache.on_commit(utf16("abc"));
    bool ok = check(!cache.valid(), "zero cap records no commits");
    cache.on_surrounding(utf16("hello world"), 11);
    ok &= check(cache.window(100) == utf16("hello world"),
                "client surrounding text remains available with zero commit history");
    return ok;
}

bool test_surrounding_limit_bounds_zero_history() {
    ContextCache cache(0);
    cache.set_surrounding_limit(4);
    const auto document = std::u16string(1000000, u'x') + u"hello world";
    cache.on_surrounding(document, document.size());
    bool ok = check(cache.window(100) == utf16("orld"),
                 "zero commit history still bounds client surrounding text");
    cache.set_surrounding_limit(2);
    ok &= check(cache.window(100) == u"ld", "shrinking zero-mode storage keeps only the tail");
    cache.set_surrounding_limit(0);
    ok &= check(!cache.valid(), "zero surrounding budget clears existing history");
    cache.on_surrounding(document, document.size());
    ok &= check(!cache.valid(), "zero surrounding budget retains nothing");
    cache.set_surrounding_limit(document.size());
    cache.on_surrounding(document, document.size());
    cache.set_surrounding_limit(4);
    ok &= check(cache.window(document.size()) == u"orld", "shrinking a large zero-mode cache keeps a bounded tail");
    cache.set_limit(document.size());
    cache.on_surrounding(document, document.size());
    cache.set_limit(4);
    ok &= check(cache.window(document.size()) == u"orld", "shrinking a large positive cache keeps a bounded tail");
    return ok;
}

bool test_surrounding_positive_history_and_boundaries() {
    ContextCache cache(8);
    cache.set_surrounding_limit(2);
    cache.on_surrounding(u"0123456789", 10);
    bool ok = check(cache.window(100) == u"23456789", "positive history ignores surrounding budget");
    cache.set_surrounding_limit(0);
    ok &= check(cache.window(100) == u"23456789", "zero surrounding budget leaves positive history alone");
    cache.on_surrounding(u"ab\U0001F600cd", 6);
    cache.set_limit(3);
    ok &= check(cache.window(100) == u"cd", "shrinking history does not split a pair");
    cache.set_limit(0);
    cache.set_surrounding_limit(3);
    cache.on_surrounding(u"ab\U0001F600cd", 6);
    ok &= check(cache.window(100) == u"cd", "zero-mode tail does not split a pair");
    cache.on_surrounding(u"ab\U0001F600cd", 3);
    ok &= check(cache.window(100) == u"ab", "cursor inside a pair drops the incomplete scalar");
    cache.on_surrounding(u"ab\xDC00z", 4);
    ok &= check(cache.window(100) == u"ab", "malformed UTF-16 retains only the valid prefix");
    return ok;
}

bool test_utf8_bounded_prefix() {
    const auto text = u16_to_utf8(u"a\U0001F600e\u0301z");
    bool ok = check(utf8_prefix_tail(text, 2, 2) == u"\U0001F600", "cursor counts scalars, not bytes or UTF-16 units");
    ok &= check(utf8_prefix_tail(text, 2, 1).empty(), "bounded conversion drops an unfittable pair");
    ok &= check(utf8_prefix_tail(text, 4, 3) == u"e\u0301", "tail boundary does not split a surrogate pair");
    ok &= check(utf8_prefix_tail(text, 100, 2) == u"\u0301z", "cursor beyond text is clamped");
    ok &= check(utf8_prefix_tail(text, 0, 100).empty(), "zero cursor has no context");
    ok &= check(utf8_prefix_tail(std::string(1000000, 'x') + text, 1000002, 2) == u"\U0001F600",
                "large document converts only the bounded prefix tail");
    for (const auto& malformed : {std::string("\xC0\xAF"), std::string("\xED\xA0\x80"),
                                  std::string("\xF4\x90\x80\x80"), std::string("\xF0\x9F"),
                                  std::string("\x80"), std::string("\xC2x")}) {
        for (size_t budget : {size_t{0}, size_t{10}}) {
            bool threw = false;
            try {
                (void)utf8_prefix_tail("ok" + malformed, 1, budget);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            ok &= check(threw, "malformed suffix rejected even outside retained context");
        }
    }
    return ok;
}

size_t code_units(char32_t scalar) { return scalar > 0xFFFF ? 2 : 1; }

size_t code_units(const std::u32string& text) {
    size_t units = 0;
    for (char32_t scalar : text) units += code_units(scalar);
    return units;
}

std::u32string to_scalars(std::u16string_view text) {
    std::u32string result;
    for (size_t i = 0; i < text.size(); ++i) {
        char32_t scalar = text[i];
        if (scalar >= 0xD800 && scalar <= 0xDBFF && i + 1 < text.size()) {
            scalar = 0x10000 + (((scalar - 0xD800) << 10U) | (text[++i] - 0xDC00));
        }
        result.push_back(scalar);
    }
    return result;
}

std::u16string to_units(const std::u32string& text) {
    std::u16string result;
    for (char32_t scalar : text) {
        if (scalar > 0xFFFF) {
            scalar -= 0x10000;
            result.push_back(static_cast<char16_t>(0xD800 + (scalar >> 10U)));
            result.push_back(static_cast<char16_t>(0xDC00 + (scalar & 0x3FFU)));
        } else {
            result.push_back(static_cast<char16_t>(scalar));
        }
    }
    return result;
}

std::u32string tail_by_units(const std::u32string& text, size_t limit) {
    std::u32string result;
    size_t units = 0;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const size_t width = code_units(*it);
        if (units + width > limit) break;
        units += width;
        result.push_back(*it);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::u32string prefix_by_units(const std::u32string& text, size_t limit) {
    size_t units = 0;
    size_t end = 0;
    while (end < text.size() && units + code_units(text[end]) <= limit) {
        units += code_units(text[end]);
        ++end;
    }
    return text.substr(0, end);
}

// Independent scalar-based model of the cache. Operations are expressed in
// scalars so the randomized comparison does not share code with the cache's
// UTF-16 boundary handling.
struct ReferenceCache {
    std::u32string history;
    size_t limit = 1024;
    size_t surrounding_limit = 1024;
    bool valid = false;

    void trim() {
        if (limit == 0) {
            history.clear();
            valid = false;
            return;
        }
        if (history.empty()) return;
        history = tail_by_units(history, limit);
        if (history.empty()) valid = false;
    }

    void rebind_surrounding() {
        if (surrounding_limit == 0) {
            history.clear();
            valid = false;
            return;
        }
        if (history.empty()) return;
        history = tail_by_units(history, surrounding_limit);
        if (history.empty()) valid = false;
    }

    void commit(std::u16string_view text) {
        if (text.empty() || limit == 0) return;
        history += to_scalars(text);
        valid = true;
        trim();
    }

    void surrounding(std::u16string_view text, size_t cursor) {
        auto scalars = to_scalars(text);
        size_t units = 0;
        size_t prefix_scalars = 0;
        while (prefix_scalars < scalars.size() && units + code_units(scalars[prefix_scalars]) <= cursor) {
            units += code_units(scalars[prefix_scalars]);
            ++prefix_scalars;
        }
        scalars.resize(prefix_scalars);
        const size_t bound = limit > 0 ? limit : surrounding_limit;
        scalars = tail_by_units(scalars, bound);
        if (scalars.empty()) {
            history.clear();
            valid = false;
            return;
        }
        history = std::move(scalars);
        valid = true;
    }

    void backspace(size_t count) {
        if (!valid || history.empty()) return;
        while (count > 0 && !history.empty()) {
            history.pop_back();
            --count;
        }
        if (history.empty()) valid = false;
    }

    std::u16string window(size_t units) const { return to_units(tail_by_units(history, units)); }
};

bool test_randomized_matches_reference() {
    ContextCache cache;
    ReferenceCache reference;
    uint32_t state = 0x1234ABCDU;
    auto next = [&state]() {
        state = state * 1664525U + 1013904223U;
        return state >> 8U;
    };
    auto random_text = [&next](size_t max_scalars) {
        std::u16string text;
        for (size_t i = 0, count = next() % max_scalars; i < count; ++i) {
            switch (next() % 5U) {
                case 0:
                    text.push_back(static_cast<char16_t>(U'a' + next() % 26U));
                    break;
                case 1:
                    text.push_back(static_cast<char16_t>(0x4E00 + next() % 0x100));
                    break;
                case 2: {
                    const char32_t scalar = 0x1F600 + next() % 0x40;
                    text.push_back(static_cast<char16_t>(0xD800 + ((scalar - 0x10000) >> 10U)));
                    text.push_back(static_cast<char16_t>(0xDC00 + ((scalar - 0x10000) & 0x3FFU)));
                    break;
                }
                default:
                    text.push_back(static_cast<char16_t>(U'0' + next() % 10U));
                    break;
            }
        }
        return text;
    };

    std::vector<std::string> trace;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        const unsigned op = next() % 8U;
        std::string step = "op" + std::to_string(op);
        if (op == 7) {
            const size_t window = next() % 15U;
            const auto got = cache.window(window);
            const auto want = reference.window(window);
            if (got != want || cache.valid() != reference.valid) {
                std::printf("[FAIL] randomized cache mismatch at iteration %d window=%zu\n", iteration, window);
                std::printf("  got  valid=%d size=%zu\n", cache.valid() ? 1 : 0, got.size());
                std::printf("  want valid=%d size=%zu\n", reference.valid ? 1 : 0, want.size());
                for (const auto& entry : trace) std::printf("  %s\n", entry.c_str());
                return false;
            }
            continue;
        }
        switch (op) {
            case 0: {
                auto text = random_text(6);
                step += " commit=" + std::to_string(text.size());
                cache.on_commit(text);
                reference.commit(text);
                break;
            }
            case 1: {
                auto text = random_text(12);
                const size_t cursor = next() % (text.size() + 1U);
                step += " surrounding=" + std::to_string(text.size()) + "/" + std::to_string(cursor);
                cache.on_surrounding(text, cursor);
                reference.surrounding(text, cursor);
                break;
            }
            case 2: {
                const size_t count = next() % 4U;
                step += " backspace=" + std::to_string(count);
                cache.on_backspace(count);
                reference.backspace(count);
                break;
            }
            case 3: {
                const size_t limit = next() % 12U;
                step += " limit=" + std::to_string(limit);
                cache.set_limit(limit);
                if (reference.limit != 0 && limit == 0) {
                    reference.history.clear();
                    reference.valid = false;
                }
                reference.limit = limit;
                if (reference.limit != 0) reference.trim();
                break;
            }
            case 4: {
                const size_t limit = next() % 12U;
                step += " surround_limit=" + std::to_string(limit);
                cache.set_surrounding_limit(limit);
                reference.surrounding_limit = limit;
                if (reference.limit == 0) reference.rebind_surrounding();
                break;
            }
            case 5:
                step += " clear";
                cache.clear();
                reference.history.clear();
                reference.valid = false;
                break;
            case 6: {
                auto text = random_text(8);
                const size_t count = next() % 3U;
                step += " commit_backspace=" + std::to_string(text.size()) + "/" + std::to_string(count);
                cache.on_commit(text);
                reference.commit(text);
                cache.on_backspace(count);
                reference.backspace(count);
                break;
            }
            default:
                break;
        }
        step += " -> cache(valid=" + std::to_string(cache.valid() ? 1 : 0) + ",size=" +
                std::to_string(cache.window(1u << 20).size()) + ") ref(valid=" +
                std::to_string(reference.valid ? 1 : 0) + ",size=" +
                std::to_string(reference.window(1u << 20).size()) + ")";
        trace.push_back(std::move(step));
        if (trace.size() > 24) trace.erase(trace.begin());
    }

    // Final full-state comparison.
    const auto got = cache.window(1u << 20);
    const auto want = reference.window(1u << 20);
    bool ok = check(got == want, "randomized cache converges to the reference");
    ok &= check(cache.valid() == reference.valid, "randomized validity converges to the reference");
    return ok;
}

}  // namespace

}  // namespace ime::fcitx5

int run_context_cache_tests() {
    using namespace ime::fcitx5;
    bool ok = true;
    ok &= test_commit_accumulates();
    ok &= test_window_truncates_oldest();
    ok &= test_commit_without_previous_history();
    ok &= test_surrounding_tail_extends_cache();
    ok &= test_surrounding_mismatch_replaces_cache();
    ok &= test_surrounding_shorter_replaces_history();
    ok &= test_surrounding_prefix_replaces_cache();
    ok &= test_surrounding_scalar_safe();
    ok &= test_surrounding_empty_resets();
    ok &= test_backspace_pops_tail();
    ok &= test_backspace_beyond_length_clears();
    ok &= test_backspace_pops_whole_scalar();
    ok &= test_clear();
    ok &= test_utf16_surrogate_handling();
    ok &= test_retention_cap_keeps_pairs_whole();
    ok &= test_retention_cap();
    ok &= test_retention_cap_zero_disables();
    ok &= test_retention_cap_resync_respects();
    ok &= test_retention_cap_set_limit();
    ok &= test_surrounding_with_zero_limit_is_kept();
    ok &= test_surrounding_limit_bounds_zero_history();
    ok &= test_surrounding_positive_history_and_boundaries();
    ok &= test_utf8_bounded_prefix();
    ok &= test_randomized_matches_reference();
    if (ok) std::printf("context cache tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
