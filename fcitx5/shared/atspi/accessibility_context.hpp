#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ime::fcitx5 {

// The most recent text before the caret observed by a context source.
struct AccessibilityContextSample {
    std::u16string text;
    std::uint64_t sequence = 0;
    bool usable = false;
};

// Samples the text before the caret of the focused editable widget through the
// AT-SPI2 accessibility bus, independently of what this IME has committed.
//
// The provider owns a worker thread running its own GLib main loop. `latest()`
// is lock-protected and cheap: the engine calls it while building a prediction
// request. Every publish advances `sequence()` even when the sample is not
// usable, so callers can detect that the focused widget changed and must not
// reuse stale text.
//
// When IME_FCITX5_DISABLE_ATSPI is set, start() refuses to run. When
// IME_FCITX5_ATSPI_SAMPLE_FILE is set, the provider reads that file (UTF-8)
// instead of connecting to the accessibility bus; this keeps the integration
// testable in headless environments.
class AccessibilityContextProvider {
public:
    explicit AccessibilityContextProvider(size_t max_code_units = 1024);
    ~AccessibilityContextProvider();
    AccessibilityContextProvider(const AccessibilityContextProvider&) = delete;
    AccessibilityContextProvider& operator=(const AccessibilityContextProvider&) = delete;

    bool start();
    void stop();
    bool running() const noexcept;

    // Collection is gated on the input method being active: while inactive the
    // backend ignores events and the latest sample becomes unusable, so the
    // focused widget's text is not retained behind the user's back.
    void set_active(bool active);
    bool active() const noexcept;

    // Asks the source to resample the focused widget. For the file-backed
    // source the file is re-read; for AT-SPI an idle callback is queued on the
    // worker loop.
    void refresh();

    void publish(std::u16string text, bool usable);

    std::optional<AccessibilityContextSample> latest() const;
    std::uint64_t sequence() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ime::fcitx5
