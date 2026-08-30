#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

void engine_test_smart_usability(fcitx::Instance* instance) {
    // Hsu Chinese is a reversible preview: Backspace edits raw keys, while a
    // following English continuation can return to the exact Latin spelling.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("nef");
        FCITX_ASSERT(harness.preedit() == "你");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit() == "ne");
        harness.type("farious");
        FCITX_ASSERT(harness.preedit() == "nefarious");
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("nefarious ", fcitx::Key(FcitxKey_space));
    });

    // OOV Latin words must not be treated as Chinese merely because the final
    // Hsu letter can also be a tone key.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("need");
        FCITX_ASSERT(harness.preedit() == "need");
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("need ", fcitx::Key(FcitxKey_space));
    });

    // A complete number is the safe preview. Chinese remains available only
    // after the user explicitly asks for alternatives with Down.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("283");
        FCITX_ASSERT(harness.preedit() == "283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "283");
        FCITX_ASSERT(harness.candidate(1) == "打");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "打");
        harness.expect_commit("打");
    });

    // A confident mixed path is previewed inline without stealing focus with
    // a candidate list. Down exposes the lossless raw alternative on demand.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello283");
        FCITX_ASSERT(harness.preedit() == "hello打");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "hello283");
        FCITX_ASSERT(harness.candidate(1) == "hello打");
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(harness.preedit() == "hello打");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(harness.preedit() == "hello283");
    });

    // Known Hsu English remains raw without an automatically opened list.
    // Return commits it in one action.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        FCITX_ASSERT(harness.preedit() == "if");
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if", fcitx::Key(FcitxKey_Return));
    });

    // An ambiguous Space remains a hidden boundary. Continuing input accepts
    // raw English and keeps the literal space without opening candidates.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("a");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.type("test");
        FCITX_ASSERT(harness.preedit() == "a test");
        harness.expect_commit("a test");
    });
}
