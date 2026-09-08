#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// SmartEnglish space decision: a pending word that is a valid first-tone
// reading converts to Chinese (top candidate into the preedit, space
// consumed), anything else commits the composition + pending word + a
// trailing space.
void engine_test_smart_space(fcitx::Instance* instance) {
    // Chinese via space (first tone): the pending word shows raw until space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        FCITX_ASSERT(harness.preedit() == "rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
    });
    // More first-tone Chinese via space (fresh harness each).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "天");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("2j");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "都");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("gji");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "說");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("tk");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "車");
    });
    // Complete numbers are safer as raw input; Down exposes Chinese readings.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("10");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "10");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "10");
        FCITX_ASSERT(harness.candidate(1) == "班");
        harness.expect_direct_commit("10 ", fcitx::Key(FcitxKey_1));
    });
    // English via space: word plus a trailing space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // English words that look bopomofo-ish still commit as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("no");
        harness.expect_direct_commit("no ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("go");
        harness.expect_direct_commit("go ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("so");
        harness.expect_direct_commit("so ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("do");
        harness.expect_direct_commit("do ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Single chars are ambiguous: English is the safe first candidate.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("a");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "a");
        harness.expect_direct_commit("a ", fcitx::Key(FcitxKey_1));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("i");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "i");
        harness.expect_direct_commit("i ", fcitx::Key(FcitxKey_1));
    });
    // Space commits the whole mixed composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("world");
        harness.expect_direct_commit("你world ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // First-tone sequence accumulates into one composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今天");
        harness.expect_commit("今天");
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Space confirms a completed preview without opening the candidate list.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
    });
    // English then first-tone Chinese in one buffer.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_direct_commit("hi ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
    });
    // Long English word.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        harness.expect_direct_commit("meeting ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
}
