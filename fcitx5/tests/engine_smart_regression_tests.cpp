#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

void engine_test_smart_regressions(fcitx::Instance* instance) {
    // SmartEnglish and Hsu must be enabled at the same time. Common English
    // words stay raw, with their Hsu interpretation available on Down.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("if");
        FCITX_ASSERT(harness.preedit() == "if");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "if");
        FCITX_ASSERT(harness.candidate(1) == "矮");
        harness.key(fcitx::Key(FcitxKey_1));
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if ", fcitx::Key(FcitxKey_space));
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("nef");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
    });

    // Standard slash is the ㄥ key and must participate in pending replay.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("x/3");
        FCITX_ASSERT(harness.preedit() == "冷");
        harness.expect_commit("冷");
    });

    // Structured ASCII is locked to English and preserves exact punctuation.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("user@example.com");
        FCITX_ASSERT(harness.preedit() == "user@example.com");
        harness.expect_direct_commit("user@example.com ", fcitx::Key(FcitxKey_space));
    });

    // Finishing an email must not bias the next token away from valid Zhuyin.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com");
        harness.expect_direct_commit("user@example.com ", fcitx::Key(FcitxKey_space));
        harness.type("283");
        FCITX_ASSERT(harness.preedit() == "283");
        harness.key(fcitx::Key(FcitxKey_Down));
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "打");
        harness.type("5j/");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "打中");
        harness.type("ru,3");
        harness.type("5.");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.type("g;4");
        harness.type("x/3");
        harness.type("-4");
        FCITX_ASSERT(harness.preedit() == "打中姐周上冷二");
        harness.expect_commit("打中姐周上冷二");
    });

    // A tone/Space boundary can switch directly from a domain to Chinese.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("gmail.com283");
        FCITX_ASSERT(harness.preedit() == "gmail.com283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "gmail.com283");
        FCITX_ASSERT(harness.candidate(1) == "gmail.com打");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "gmail.com打");
        harness.type("5j/");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "gmail.com打中");
        harness.expect_commit("gmail.com打中");
    });

    // Locked Email/URL tokens retain the same parallel suffix alternatives.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "user@example.com283");
        FCITX_ASSERT(harness.candidate(1) == "user@example.com打");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("user@example.com打");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("https://example.com5j/");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "https://example.com5j/");
        FCITX_ASSERT(harness.candidate(1) == "https://example.com中");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("https://example.com中");
    });

    // A known plain-English word can switch without an intervening separator.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("hello283");
        FCITX_ASSERT(harness.preedit() == "hello打");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "hello283");
        FCITX_ASSERT(harness.candidate(1) == "hello打");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "hello打");
        harness.type("5j/");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "hello打中");
        harness.expect_commit("hello打中");
    });

    // A long uninterrupted Latin token is safer as English than as a possible
    // all-letter first-tone suffix.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("hellorup");
        harness.expect_direct_commit("hellorup ", fcitx::Key(FcitxKey_space));
    });

    // Suffix parsing is grammatical and does not depend on the English lexicon.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("qwerty283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "qwerty283");
        FCITX_ASSERT(harness.candidate(1) == "qwerty打");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("qwerty打");
    });

    // A complete structured token followed by Space remains plain English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("gmail.com");
        harness.expect_direct_commit("gmail.com ", fcitx::Key(FcitxKey_space));
    });

    // Intrinsically ambiguous English+tone input exposes both complete results.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello4");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "hello4");
        FCITX_ASSERT(harness.candidate(1) == "hell欸");
        harness.key(fcitx::Key(FcitxKey_1));
        harness.expect_direct_commit("hello4 ", fcitx::Key(FcitxKey_space));
    });

    // Hsu uses the same parallel suffix decision with letter tone keys.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("gmail.comdyf");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "gmail.comdyf");
        FCITX_ASSERT(harness.candidate(1) == "gmail.com打");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("gmail.com打");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("gmail.comjxl");
        FCITX_ASSERT(harness.preedit() == "gmail.comjxl");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "gmail.comjxl");
        FCITX_ASSERT(harness.candidate(1) == "gmail.com中");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("gmail.com中");
    });

    // Alphabetic selection keys must not consume continued Hsu English input.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "本位列"}});
        harness.type("hda");
        FCITX_ASSERT(harness.preedit() == "hda");
        harness.expect_direct_commit("hda ", fcitx::Key(FcitxKey_space));
    });

    // The same token reset must hold for Hsu's letter-based tone keys.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com");
        harness.expect_direct_commit("user@example.com ", fcitx::Key(FcitxKey_space));
        harness.type("dyf");
        FCITX_ASSERT(harness.preedit() == "打");
        harness.type("jxl");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "打中");
        harness.expect_commit("打中");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com/v1.2.3");
        FCITX_ASSERT(harness.preedit() == "https://example.com/v1.2.3");
        harness.expect_direct_commit("https://example.com/v1.2.3 ", fcitx::Key(FcitxKey_space));
    });

    // A known English token commits directly instead of opening ambiguity.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("ai");
        harness.expect_direct_commit("ai ", fcitx::Key(FcitxKey_space));
    });

    // Single-key first-tone readings are no longer forced to English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("u");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "u");
        FCITX_ASSERT(harness.candidate(1) == "一");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "一");
        harness.expect_commit("一");
    });

    // Configured alphabetic selection keys retain priority over SmartEnglish.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "True"}, {"SelectionKeys", "本位列"}});
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_a));
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
    });

    // FocusOut must not silently lose captured English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_focus_out_commit("hello");
        harness.input_context()->focusIn();
    });

    // Continuing after an ambiguous Space accepts the raw path and preserves
    // the word boundary instead of joining the two words together.
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

    // Backspace after an ambiguous Space cancels that unconfirmed boundary;
    // it must not immediately delete the preceding pending character.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("a");
        harness.key(fcitx::Key(FcitxKey_space));
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit() == "a");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // Printable punctuation continues the raw ASCII token while mixed
    // alternatives remain hidden.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        FCITX_ASSERT(!harness.has_candidates());
        harness.type(".");
        FCITX_ASSERT(harness.preedit() == "if.");
        harness.expect_direct_commit("if. ", fcitx::Key(FcitxKey_space));
    });

    // Return confirms the raw preview in one action.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if", fcitx::Key(FcitxKey_Return));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // Navigation settles pending ASCII into the composition. A later
    // FocusOut must still commit that text instead of silently clearing it.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello");
        harness.key(fcitx::Key(FcitxKey_Left));
        FCITX_ASSERT(harness.preedit() == "hello");
        harness.expect_focus_out_commit("hello");
        harness.input_context()->focusIn();
    });

    // Once pending ASCII has been settled for navigation, printable input in
    // an all-literal composition is inserted at the caret, not appended to the
    // end through a detached pending token.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello");
        harness.key(fcitx::Key(FcitxKey_Left));
        harness.type("x");
        FCITX_ASSERT(harness.preedit() == "hellxo");
        harness.expect_commit("hellxo");
    });

    // Existing CapsLock+Shift English intent keeps priority over SmartEnglish.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "True"}, {"CapsLockInputsBopomofo", "True"}});
        harness.type("su3");
        harness.expect_direct_commit(
            "你A", fcitx::Key(FcitxKey_a,
                               fcitx::KeyStates(fcitx::KeyState::CapsLock) | fcitx::KeyState::Shift));
    });
}
