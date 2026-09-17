#include "fcitx5/input_context_property.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

void engine_test_async_state_tests(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([]() {
        ime::fcitx5::ImeInputContextProperty first;
        ime::fcitx5::ImeInputContextProperty second;

        first.session.prediction.revision = 7;
        second.session.prediction.revision = 19;
        FCITX_ASSERT(first.session.prediction.revision == 7);
        FCITX_ASSERT(second.session.prediction.revision == 19);

        // Dirty marking only applies while a request is in flight, and each
        // input context owns its own prediction state.
        first.session.prediction.mark_dirty();
        FCITX_ASSERT(!first.session.prediction.dirty);
        (void)first.session.prediction.begin({0}, u"key", 1);
        first.session.prediction.mark_dirty();
        FCITX_ASSERT(first.session.prediction.dirty);
        FCITX_ASSERT(!second.session.prediction.dirty);
    });
}
