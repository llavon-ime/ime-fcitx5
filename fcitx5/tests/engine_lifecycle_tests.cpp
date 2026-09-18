#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

using namespace ime::fcitx5::test;

void engine_test_lifecycle(fcitx::Instance* instance) {
    // Focus-out commits a complete composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_focus_out_commit("你");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // Focus-out also commits an unresolved smart-English pending token.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_focus_out_commit("hello");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // An unfinished bopomofo reading is discarded rather than committed.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        harness.input_context()->focusOut();
        FCITX_ASSERT(harness.preedit().empty());
        FCITX_ASSERT(harness.engine_state()->session.context_text.empty());
    });

    // An explicit client reset never commits, even with a complete segment.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        auto* state = harness.engine_state();
        FCITX_ASSERT(state != nullptr);
        const auto generation = state->session.prediction.generation;
        harness.input_context()->reset();
        FCITX_ASSERT(harness.preedit().empty());
        FCITX_ASSERT(state->session.empty());
        FCITX_ASSERT(state->session.buffer.empty());
        FCITX_ASSERT(state->session.context_text.empty());
        FCITX_ASSERT(state->session.prediction.generation == generation + 1);
    });

    // Config changes settle exact pending keys as literals, clear the mixed
    // decision, invalidate prediction, and close an existing model session.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        auto* state = harness.engine_state();
        FCITX_ASSERT(state != nullptr);
        ime::fcitx5::protocol::SessionId session_id{};
        session_id[0] = 0x44;
        state->session.prediction.session_id = session_id;
        const auto generation = state->session.prediction.generation;
        state->session_close_handle = []() {};

        harness.set_config("SmartEnglish", "False");
        FCITX_ASSERT(state->session.pending_token.empty());
        FCITX_ASSERT(!state->session.mixed_decision.active());
        FCITX_ASSERT(state->session.buffer.commit_text() == std::u16string(u"hello"));
        FCITX_ASSERT(!state->session.prediction.session_open());
        FCITX_ASSERT(state->session.prediction.generation == generation + 1);
        FCITX_ASSERT(!state->session_close_handle);
    });
}
