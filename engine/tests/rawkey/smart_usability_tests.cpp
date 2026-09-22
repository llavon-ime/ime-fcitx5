#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("smart usability", engine_test_smart_usability) {
    // Hsu Chinese is a reversible preview: Backspace edits raw keys, while a
    // following English continuation can return to the exact Latin spelling.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "你");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "ne");
        harness.type("farious");
        RAWKEY_ASSERT(harness.preedit() == "nefarious");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("nefarious ", Key(" "));
}

    // OOV Latin words must not be treated as Chinese merely because the final
    // Hsu letter can also be a tone key.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("need");
        RAWKEY_ASSERT(harness.preedit() == "need");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("need ", Key(" "));
}

    // A complete number is the safe preview. Chinese remains available only
    // after the user explicitly asks for alternatives with Down.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("283");
        RAWKEY_ASSERT(harness.preedit() == "283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "283");
        RAWKEY_ASSERT(harness.candidate(1) == "打");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "打");
        harness.expect_commit("打");
}

    // A confident mixed path is previewed inline without stealing focus with
    // a candidate list. Down exposes the lossless raw alternative on demand.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello283");
        RAWKEY_ASSERT(harness.preedit() == "hello打");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "hello283");
        RAWKEY_ASSERT(harness.candidate(1) == "hello打");
        harness.key(Key("Escape"));
        RAWKEY_ASSERT(harness.preedit() == "hello打");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Escape"));
        RAWKEY_ASSERT(harness.preedit() == "hello283");
}

    // Known Hsu English remains raw without an automatically opened list.
    // Return commits it in one action.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("if");
        RAWKEY_ASSERT(harness.preedit() == "if");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("if", Key("Return"));
}

    // An ambiguous Space remains a hidden boundary. Continuing input accepts
    // raw English and keeps the literal space without opening candidates.
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
}
