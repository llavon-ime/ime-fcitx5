#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

namespace {

int cursor_index(const EngineHarness& harness) {
    const auto list = harness.input_context()->inputPanel().candidateList();
    return list ? list->cursorIndex() : -1;
}

}  // namespace

// Candidate list interaction after a Chinese preview is explicitly confirmed
// and opened with Down (SmartEnglish ON).
void engine_test_smart_candidate(fcitx::Instance* instance) {
    // 1. Confirm the preview with Space, then open candidates with Down.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate_count() > 1);
    });

    // 2. Select a non-top candidate with a digit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "你");
        const std::string second = harness.candidate(1);
        FCITX_ASSERT(!second.empty());
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == second);
        harness.expect_commit(second);
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 3. Navigate the candidate list after conversion: Down moves the cursor,
    // a second Space (space_selects_candidate, the default) selects it.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(cursor_index(harness) == 0);
        const std::string second = harness.candidate(1);
        FCITX_ASSERT(!second.empty());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(cursor_index(harness) == 1);
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(harness.preedit() == second);
        harness.expect_commit(second);
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 4. Escape closes the candidate list, keeping the converted Chinese.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(harness.preedit() == "你");
    });

    // 5. English after selecting a Chinese candidate: the digit selects the
    // candidate into the preedit, Return commits it, then the next pending
    // word types English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        const std::string top = harness.candidate(0);
        harness.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(harness.preedit() == top);
        harness.expect_commit(top);
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("ok");
        FCITX_ASSERT(harness.preedit() == "ok");
        harness.expect_direct_commit("ok ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 6. A space-converted syllable opens candidates only on Down.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        FCITX_ASSERT(harness.preedit() == "rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
    });

    // 7. Select a candidate from a space-converted syllable.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.expect_commit("今");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 8. An English pending word never opens the candidate list on space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        FCITX_ASSERT(harness.preedit() == "hello");
        harness.expect_direct_commit("hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        FCITX_ASSERT(!harness.has_candidates());
    });

    // 9. Return commits Chinese directly after the tone conversion.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "好");
        harness.expect_commit("好");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 10. Letters while the candidate list is open close it and start a new
    // pending word appended to the composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        harness.type("hi");
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(harness.preedit() == "你hi");
    });
}
