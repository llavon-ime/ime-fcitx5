#pragma once

#include <fcitx/inputcontextproperty.h>

#include <cstdint>
#include <functional>

#include "host/host.hpp"
#include "input/input_session.hpp"

namespace llavon::ime {

// Per-input-context engine handle. The engine owns the InputSession; this
// property carries the ContextId, a raw session pointer for tests, and the
// detach hook that runs when the fcitx input context is destroyed.
class ImeInputContextProperty final : public fcitx::InputContextProperty {
public:
    ~ImeInputContextProperty() override {
        if (on_destroy) on_destroy();
    }

    ContextId id = 0;
    // Owned by the engine; valid while this context is attached.
    InputSession* session = nullptr;
    std::function<void()> on_destroy;

    void copyTo(fcitx::InputContextProperty*) override {}
    bool needCopy() const override { return false; }
};

using ImeInputContextPropertyFactory = fcitx::SimpleInputContextPropertyFactory<ImeInputContextProperty>;

}  // namespace llavon::ime
