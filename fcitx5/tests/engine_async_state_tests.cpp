#include "fcitx5/input_context_property.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

void engine_test_async_state_tests(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([]() {
        ime::fcitx5::ImeInputContextProperty first;
        ime::fcitx5::ImeInputContextProperty second;

        first.prediction_revision = 7;
        second.prediction_revision = 19;
        FCITX_ASSERT(first.prediction_revision == 7);
        FCITX_ASSERT(second.prediction_revision == 19);

        bool dirty = false;
        FCITX_ASSERT(ime::fcitx5::prediction_change_requires_request(false, dirty));
        FCITX_ASSERT(!dirty);
        FCITX_ASSERT(!ime::fcitx5::prediction_change_requires_request(true, dirty));
        FCITX_ASSERT(dirty);
    });
}
