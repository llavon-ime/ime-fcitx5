#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// SmartEnglish space decision: a pending word that is a valid first-tone
// reading converts to Chinese (top candidate into the preedit, space
// consumed), anything else commits the composition + pending word + a
// trailing space.
RAWKEY_SUITE("smart space", engine_test_smart_space) {
    // Chinese via space (first tone): the pending word shows raw until space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        RAWKEY_ASSERT(harness.preedit() == "rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
}
    // More first-tone Chinese via space (fresh harness each).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "天");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("2j");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "都");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("gji");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "說");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("tk");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "車");
}
    // Complete numbers are safer as raw input; Down exposes Chinese readings.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("10");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "10");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "10");
        RAWKEY_ASSERT(harness.candidate(1) == "班");
        harness.expect_direct_commit("10 ", Key("1"));
}
    // English via space: word plus a trailing space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // English words that look bopomofo-ish still commit as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("no");
        harness.expect_direct_commit("no ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("go");
        harness.expect_direct_commit("go ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("so");
        harness.expect_direct_commit("so ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("do");
        harness.expect_direct_commit("do ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Single chars are ambiguous: English is the safe first candidate.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("a");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "a");
        harness.expect_direct_commit("a ", Key("1"));
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("i");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "i");
        harness.expect_direct_commit("i ", Key("1"));
}
    // Space commits the whole mixed composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("world");
        harness.expect_direct_commit("你world ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // First-tone sequence accumulates into one composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今天");
        harness.expect_commit("今天");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Space confirms a completed preview without opening the candidate list.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
}
    // English then first-tone Chinese in one buffer.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_direct_commit("hi ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
}
    // Long English word.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        harness.expect_direct_commit("meeting ", Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
}
