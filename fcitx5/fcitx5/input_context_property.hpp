#pragma once

#include <fcitx/inputcontextproperty.h>

#include <functional>

#include "input/input_session.hpp"

namespace ime::fcitx5 {

class ImeInputContextProperty final : public fcitx::InputContextProperty {
public:
    ~ImeInputContextProperty() override {
        if (session_close_handle) session_close_handle();
    }

    InputSession session;

    std::function<void()> session_close_handle;

    void copyTo(fcitx::InputContextProperty*) override {}
    bool needCopy() const override { return false; }
};

using ImeInputContextPropertyFactory = fcitx::SimpleInputContextPropertyFactory<ImeInputContextProperty>;

}  // namespace ime::fcitx5
