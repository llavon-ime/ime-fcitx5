#pragma once

#include "context/accessibility_context.hpp"

#include <memory>

namespace ime::fcitx5 {

// AT-SPI2 backend: samples the focused editable widget through the
// accessibility bus with a worker thread running its own GLib main loop.
class AtspiContextProvider final : public AccessibilityContextProvider {
public:
    explicit AtspiContextProvider(size_t max_code_units);
    ~AtspiContextProvider() override;

    AtspiContextProvider(const AtspiContextProvider&) = delete;
    AtspiContextProvider& operator=(const AtspiContextProvider&) = delete;

    bool start() override;
    void stop() override;
    bool running() const noexcept override;
    void refresh() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ime::fcitx5
