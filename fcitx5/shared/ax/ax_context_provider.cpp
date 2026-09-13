#include "ax/ax_context_provider.hpp"

// AXIsProcessTrusted is a Core Foundation API exported by ApplicationServices.
// Declared here so the abstraction layer stays a plain C++ translation unit.
extern "C" unsigned char AXIsProcessTrusted(void);

namespace ime::fcitx5 {

bool AxContextProvider::start() {
    running_.store(false);
    if (!AXIsProcessTrusted()) {
        set_availability(AccessibilityAvailability::Unavailable, "ax-permission-required");
        return false;
    }
    set_availability(AccessibilityAvailability::Unavailable, "ax-sampling-not-implemented");
    return false;
}

void AxContextProvider::stop() { running_.store(false); }

bool AxContextProvider::running() const noexcept { return running_.load(); }

void AxContextProvider::refresh() {}

}  // namespace ime::fcitx5
