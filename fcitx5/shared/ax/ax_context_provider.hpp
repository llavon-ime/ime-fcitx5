#pragma once

#include "context/accessibility_context.hpp"

#include <atomic>

namespace ime::fcitx5 {

// macOS backend. This is currently the abstraction layer only: it reports
// whether the process is trusted for the Accessibility API. Sampling the text
// before the caret with AXUIElement is the next step.
class AxContextProvider final : public AccessibilityContextProvider {
public:
    using AccessibilityContextProvider::AccessibilityContextProvider;

    bool start() override;
    void stop() override;
    bool running() const noexcept override;
    void refresh() override;

private:
    std::atomic<bool> running_{false};
};

}  // namespace ime::fcitx5
