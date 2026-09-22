#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("smart regressions", engine_test_smart_regressions) {
    // SmartEnglish and Hsu must be enabled at the same time. Common English
    // words stay raw, with their Hsu interpretation available on Down.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("if");
        RAWKEY_ASSERT(harness.preedit() == "if");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "if");
        RAWKEY_ASSERT(harness.candidate(1) == "矮");
        harness.key(Key("1"));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if ", Key(" "));
}

    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
}

    // Standard slash is the ㄥ key and must participate in pending replay.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("x/3");
        RAWKEY_ASSERT(harness.preedit() == "冷");
        harness.expect_commit("冷");
}

    // Structured ASCII is locked to English and preserves exact punctuation.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("user@example.com");
        RAWKEY_ASSERT(harness.preedit() == "user@example.com");
        harness.expect_direct_commit("user@example.com ", Key(" "));
}

    // Finishing an email must not bias the next token away from valid Zhuyin.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com");
        harness.expect_direct_commit("user@example.com ", Key(" "));
        harness.type("283");
        RAWKEY_ASSERT(harness.preedit() == "283");
        harness.key(Key("Down"));
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "打");
        harness.type("5j/");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "打中");
        harness.type("ru,3");
        harness.type("5.");
        harness.key(Key(" "));
        harness.type("g;4");
        harness.type("x/3");
        harness.type("-4");
        RAWKEY_ASSERT(harness.preedit() == "打中姐周上冷二");
        harness.expect_commit("打中姐周上冷二");
}

    // A tone/Space boundary can switch directly from a domain to Chinese.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("gmail.com283");
        RAWKEY_ASSERT(harness.preedit() == "gmail.com283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "gmail.com283");
        RAWKEY_ASSERT(harness.candidate(1) == "gmail.com打");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "gmail.com打");
        harness.type("5j/");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "gmail.com打中");
        harness.expect_commit("gmail.com打中");
}

    // Locked Email/URL tokens retain the same parallel suffix alternatives.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "user@example.com283");
        RAWKEY_ASSERT(harness.candidate(1) == "user@example.com打");
        harness.key(Key("2"));
        harness.expect_commit("user@example.com打");
}

    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("https://example.com5j/");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "https://example.com5j/");
        RAWKEY_ASSERT(harness.candidate(1) == "https://example.com中");
        harness.key(Key("2"));
        harness.expect_commit("https://example.com中");
}

    // A known plain-English word can switch without an intervening separator.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("hello283");
        RAWKEY_ASSERT(harness.preedit() == "hello打");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "hello283");
        RAWKEY_ASSERT(harness.candidate(1) == "hello打");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "hello打");
        harness.type("5j/");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "hello打中");
        harness.expect_commit("hello打中");
}

    // A long uninterrupted Latin token is safer as English than as a possible
    // all-letter first-tone suffix.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("hellorup");
        harness.expect_direct_commit("hellorup ", Key(" "));
}

    // Suffix parsing is grammatical and does not depend on the English lexicon.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"}, {"SmartEnglish", "True"}});
        harness.type("qwerty283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "qwerty283");
        RAWKEY_ASSERT(harness.candidate(1) == "qwerty打");
        harness.key(Key("2"));
        harness.expect_commit("qwerty打");
}

    // A complete structured token followed by Space remains plain English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("gmail.com");
        harness.expect_direct_commit("gmail.com ", Key(" "));
}

    // Intrinsically ambiguous English+tone input exposes both complete results.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello4");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "hello4");
        RAWKEY_ASSERT(harness.candidate(1) == "hell欸");
        harness.key(Key("1"));
        harness.expect_direct_commit("hello4 ", Key(" "));
}

    // Hsu uses the same parallel suffix decision with letter tone keys.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("gmail.comdyf");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "gmail.comdyf");
        RAWKEY_ASSERT(harness.candidate(1) == "gmail.com打");
        harness.key(Key("2"));
        harness.expect_commit("gmail.com打");
}

    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("gmail.comjxl");
        RAWKEY_ASSERT(harness.preedit() == "gmail.comjxl");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "gmail.comjxl");
        RAWKEY_ASSERT(harness.candidate(1) == "gmail.com中");
        harness.key(Key("2"));
        harness.expect_commit("gmail.com中");
}

    // Alphabetic selection keys must not consume continued Hsu English input.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "本位列"}});
        harness.type("hda");
        RAWKEY_ASSERT(harness.preedit() == "hda");
        harness.expect_direct_commit("hda ", Key(" "));
}

    // The same token reset must hold for Hsu's letter-based tone keys.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("user@example.com");
        harness.expect_direct_commit("user@example.com ", Key(" "));
        harness.type("dyf");
        RAWKEY_ASSERT(harness.preedit() == "打");
        harness.type("jxl");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "打中");
        harness.expect_commit("打中");
}

    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com/v1.2.3");
        RAWKEY_ASSERT(harness.preedit() == "https://example.com/v1.2.3");
        harness.expect_direct_commit("https://example.com/v1.2.3 ", Key(" "));
}

    // A known English token commits directly instead of opening ambiguity.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ai");
        harness.expect_direct_commit("ai ", Key(" "));
}

    // Single-key first-tone readings are no longer forced to English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("u");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "u");
        RAWKEY_ASSERT(harness.candidate(1) == "一");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "一");
        harness.expect_commit("一");
}

    // Configured alphabetic selection keys retain priority over SmartEnglish.
    {
        Harness harness;
        harness.set_configs({{"SmartEnglish", "True"}, {"SelectionKeys", "本位列"}});
        harness.type("su3");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        harness.key(Key("a"));
        RAWKEY_ASSERT(!harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
}

    // FocusOut must not silently lose captured English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_focus_out_commit("hello");
        harness.activate();
}

    // Continuing after an ambiguous Space accepts the raw path and preserves
    // the word boundary instead of joining the two words together.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("a");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.type("test");
        RAWKEY_ASSERT(harness.preedit() == "a test");
        harness.expect_commit("a test");
}

    // Backspace after an ambiguous Space cancels that unconfirmed boundary;
    // it must not immediately delete the preceding pending character.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("a");
        harness.key(Key(" "));
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "a");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // Printable punctuation continues the raw ASCII token while mixed
    // alternatives remain hidden.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.type(".");
        RAWKEY_ASSERT(harness.preedit() == "if.");
        harness.expect_direct_commit("if. ", Key(" "));
}

    // Return confirms the raw preview in one action.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if", Key("Return"));
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // Navigation settles pending ASCII into the composition. A later
    // FocusOut must still commit that text instead of silently clearing it.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello");
        harness.key(Key("Left"));
        RAWKEY_ASSERT(harness.preedit() == "hello");
        harness.expect_focus_out_commit("hello");
        harness.activate();
}

    // Once pending ASCII has been settled for navigation, printable input in
    // an all-literal composition is inserted at the caret, not appended to the
    // end through a detached pending token.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello");
        harness.key(Key("Left"));
        harness.type("x");
        RAWKEY_ASSERT(harness.preedit() == "hellxo");
        harness.expect_commit("hellxo");
}

    // Existing CapsLock+Shift English intent keeps priority over SmartEnglish.
    {
        Harness harness;
        harness.set_configs({{"SmartEnglish", "True"}, {"CapsLockInputsBopomofo", "True"}});
        harness.type("su3");
        // The frontends resolve Shift XOR CapsLock into the keysym case, so
        // CapsLock alone arrives lowercase and types English.
        harness.expect_direct_commit("你A", Key("a").with(kCapsLock));
}
}
