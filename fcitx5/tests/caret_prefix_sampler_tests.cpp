#include "atspi/caret_prefix_sampler.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace ime::fcitx5 {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

bool test_simple_prefix() {
    CaretPrefixSampler sampler(80);
    std::u16string text = u"今天天氣很好，現在繼續寫";
    sampler.set_text(std::move(text), 6);
    bool ok = check(sampler.text_before_caret() == u"今天天氣很好", "caret splits the text correctly");
    ok &= check(sampler.window_start() == 0, "full sample starts at zero");
    ok &= check(sampler.sampled() && sampler.usable(), "sample is usable");
    return ok;
}

bool test_multiline_prefix_is_kept() {
    // AT-SPI gives the whole widget text, so earlier lines are real context.
    CaretPrefixSampler sampler(80);
    std::u16string text = u"第一行\n第二行內容";
    sampler.set_text(std::move(text), 10);
    bool ok = check(sampler.usable(), "multi-line sample is usable");
    ok &= check(sampler.text_before_caret() == u"第一行\n第二行內容", "all text before the caret is kept");
    return ok;
}

bool test_window_trim_drops_oldest() {
    CaretPrefixSampler sampler(5);
    std::u16string text = u"abcdefghij";
    sampler.set_text(std::move(text), 10);
    bool ok = check(sampler.usable(), "long sample is usable");
    ok &= check(sampler.window_start() == 5, "window_start accounts for the dropped prefix");
    ok &= check(sampler.text_before_caret() == u"fghij", "text is truncated to the newest code units");
    return ok;
}

bool test_no_text_not_sampled() {
    CaretPrefixSampler sampler(80);
    sampler.set_text(std::u16string(), 0);
    return check(!sampler.sampled() && !sampler.usable(), "no text means not sampled");
}

bool test_empty_pre_caret() {
    CaretPrefixSampler sampler(80);
    sampler.set_text(std::u16string(u"rest"), 0);
    return check(sampler.usable() && sampler.text_before_caret().empty(),
                 "caret at text start is usable with an empty prefix");
}

bool test_caret_past_end_clamped() {
    CaretPrefixSampler sampler(80);
    std::u16string text = u"abc";
    sampler.set_text(std::move(text), 99);
    bool ok = check(sampler.usable(), "overlong caret clamps");
    ok &= check(sampler.text_before_caret() == u"abc", "clamped caret takes the whole text");
    return ok;
}

}  // namespace
}  // namespace ime::fcitx5

int run_caret_prefix_sampler_tests() {
    using namespace ime::fcitx5;
    bool ok = true;
    ok &= test_simple_prefix();
    ok &= test_multiline_prefix_is_kept();
    ok &= test_window_trim_drops_oldest();
    ok &= test_no_text_not_sampled();
    ok &= test_empty_pre_caret();
    ok &= test_caret_past_end_clamped();
    if (ok) std::printf("caret prefix sampler tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
