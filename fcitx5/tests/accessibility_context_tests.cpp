#include "atspi/accessibility_context.hpp"
#include "text/utf.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ime::fcitx5 {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* saved = std::getenv(name)) saved_ = std::string(saved);
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (saved_) {
            setenv(name_.c_str(), saved_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

bool test_publish_and_sequence() {
    AccessibilityContextProvider provider(64);
    bool ok = check(!provider.latest().has_value(), "no sample before the first publish");
    provider.publish(u"你好", true);
    const auto first = provider.latest();
    ok &= check(first.has_value() && first->usable && first->text == u"你好", "publish exposes the sample");
    ok &= check(first->sequence == 1, "the first sample has sequence 1");
    provider.publish(std::u16string(), false);
    const auto second = provider.latest();
    ok &= check(second.has_value() && !second->usable, "an unusable sample still advances the sequence");
    ok &= check(second->sequence == 2, "every publish advances the sequence");
    ok &= check(provider.sequence() == 2, "sequence() reports the latest sequence");
    return ok;
}

bool test_disabled_start() {
    ScopedEnv disable("IME_FCITX5_DISABLE_ATSPI", "1");
    ScopedEnv sample("IME_FCITX5_ATSPI_SAMPLE_FILE", nullptr);
    AccessibilityContextProvider provider(64);
    bool ok = check(!provider.start(), "start() refuses while AT-SPI is disabled");
    ok &= check(!provider.running(), "a refused provider is not running");
    return ok;
}

bool test_file_backed_sample() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-atspi-sample-test.txt";
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary);
        output << "\xe6\x97\xa9\xe5\xae\x89\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c";
    }
    ScopedEnv sample("IME_FCITX5_ATSPI_SAMPLE_FILE", path.c_str());
    ScopedEnv disable("IME_FCITX5_DISABLE_ATSPI", nullptr);

    AccessibilityContextProvider provider(64);
    bool ok = check(provider.start(), "the file-backed source starts headlessly");
    ok &= check(provider.running(), "the file-backed source reports running");
    const auto first = provider.latest();
    ok &= check(first.has_value() && first->usable && first->text == u"早安，世界",
                "the file content becomes the sample");
    ok &= check(first->sequence == 1, "the initial file read publishes sequence 1");
    provider.set_active(true);

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "\xe7\xac\xac\xe4\xba\x8c\xe6\xae\xb5\xe6\x96\x87\xe5\xad\x97";
    }
    provider.refresh();
    const auto second = provider.latest();
    ok &= check(second.has_value() && second->usable && second->text == u"第二段文字",
                "refresh re-reads the file");
    ok &= check(second->sequence == 2, "refresh advances the sequence");

    std::filesystem::remove(path);
    provider.refresh();
    const auto missing = provider.latest();
    ok &= check(missing.has_value() && !missing->usable, "a missing file publishes an unusable sample");
    ok &= check(missing->sequence == 3, "the unusable sample still advances the sequence");

    provider.stop();
    ok &= check(!provider.running(), "stop() ends the file-backed source");
    return ok;
}

bool test_file_sample_bounded_utf16() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-atspi-window-test.txt";
    std::string text;
    for (int i = 0; i < 100; ++i) text += "ab";
    text += "\xF0\x9F\x98\x80";  // U+1F600
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }
    ScopedEnv sample("IME_FCITX5_ATSPI_SAMPLE_FILE", path.c_str());

    AccessibilityContextProvider provider(5);
    (void)provider.start();
    const auto latest = provider.latest();
    bool ok = check(latest.has_value() && latest->usable, "a long file sample is usable");
    ok &= check(latest->text.size() <= 5, "the file sample respects the code-unit bound");
    ok &= check(latest->text == u"bab\U0001F600", "the sample keeps the newest complete scalars");
    std::filesystem::remove(path);
    return ok;
}

bool test_active_gating() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-atspi-active-test.txt";
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "active sample";
    }
    ScopedEnv sample("IME_FCITX5_ATSPI_SAMPLE_FILE", path.c_str());

    AccessibilityContextProvider provider(64);
    bool ok = check(provider.start(), "the provider starts for the active-gating test");
    ok &= check(!provider.active(), "providers start inactive");
    ok &= check(provider.sequence() == 1, "the initial file read publishes once");

    provider.set_active(false);
    ok &= check(provider.sequence() == 1, "deactivating while already inactive publishes nothing");
    provider.set_active(true);
    ok &= check(provider.active(), "set_active(true) marks the provider active");
    ok &= check(provider.sequence() == 1, "activating alone does not publish");
    provider.refresh();
    ok &= check(provider.sequence() == 2, "refresh publishes while active");

    provider.set_active(false);
    const auto inactive = provider.latest();
    ok &= check(inactive.has_value() && !inactive->usable, "deactivating invalidates the sample");
    ok &= check(inactive->sequence == 3, "deactivation advances the sequence");
    provider.refresh();
    ok &= check(provider.sequence() == 3, "refresh does nothing while inactive");

    provider.set_active(true);
    provider.refresh();
    const auto resumed = provider.latest();
    ok &= check(resumed.has_value() && resumed->usable && resumed->text == u"active sample",
                "reactivating and refreshing restores a usable sample");
    std::filesystem::remove(path);
    return ok;
}

bool test_concurrent_access() {
    AccessibilityContextProvider provider(64);
    std::atomic<bool> stop{false};
    std::thread reader([&provider, &stop]() {
        while (!stop.load()) {
            const auto sample = provider.latest();
            if (sample.has_value() && sample->sequence == 0) std::printf("[FAIL] sequence regression\n");
        }
    });
    for (int i = 0; i < 1000; ++i) provider.publish(u"並行", i % 2 == 0);
    stop.store(true);
    reader.join();
    return check(provider.sequence() == 1000, "concurrent publishes keep a monotonic sequence");
}

}  // namespace
}  // namespace ime::fcitx5

int run_accessibility_context_tests() {
    using namespace ime::fcitx5;
    bool ok = true;
    ok &= test_publish_and_sequence();
    ok &= test_disabled_start();
    ok &= test_file_backed_sample();
    ok &= test_file_sample_bounded_utf16();
    ok &= test_active_gating();
    ok &= test_concurrent_access();
    if (ok) std::printf("accessibility context tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
