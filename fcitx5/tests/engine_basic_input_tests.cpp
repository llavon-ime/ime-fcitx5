#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// Types "su3" (ㄋㄧˇ in the standard layout), opens candidates with Space,
// selects with a digit, and commits with Return.
void engine_test_basic_input(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        FCITX_ASSERT(harness.input_context());

        harness.type("su3");
        // Completed syllable converts to the top candidate immediately.
        FCITX_ASSERT(harness.preedit() == "你");

        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.has_candidates());
        const std::string first = harness.candidate(0);
        FCITX_ASSERT(!first.empty());

        // 1 selects the first candidate.
        harness.key(fcitx::Key(FcitxKey_1));
        harness.expect_commit(first);
    });
}
