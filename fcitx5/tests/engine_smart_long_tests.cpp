#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// SmartEnglish: long and multi-syllable compositions plus consecutive English
// words. Each scenario builds a fresh EngineHarness and enables the
// SmartEnglish config option explicitly (config is global across tests).
void engine_test_smart_long(fcitx::Instance* instance) {
    // 1. Three-syllable Chinese: 你(ㄋㄧˇ)+好(ㄏㄠˇ)+我(ㄨㄛˇ), tone keys
    // decide each syllable; preedit shows the composition, Return commits it.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        harness.type("ji3");
        FCITX_ASSERT(harness.preedit() == "你好我");
        harness.expect_commit("你好我");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 2. First-tone sentence: 今(ㄐㄧㄣ)+天(ㄊㄧㄢ) built from first-tone space
    // decisions (the space converts the pending word to Chinese in the
    // preedit, it does not commit), then 好 via a tone key; Return commits.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今天");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "今天好");
        harness.expect_commit("今天好");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 3. Long English word held as a pending word, shown raw in the preedit;
    // space commits it as English with a trailing space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("deadline");
        FCITX_ASSERT(harness.preedit() == "deadline");
        harness.expect_direct_commit("deadline ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 4. Two consecutive English words in one harness: each word + space
    // commits that word (with trailing space), leaving an empty buffer.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        FCITX_ASSERT(harness.preedit() == "meeting");
        harness.expect_direct_commit("meeting ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("report");
        FCITX_ASSERT(harness.preedit() == "report");
        harness.expect_direct_commit("report ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 5. Mixed long line: Chinese composition 會(ㄏㄨㄟˋ)上(ㄕㄤˋ)的(ㄉㄜ˙),
    // then a pending English word; space commits everything in one commit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("cjo4");
        harness.type("g;4");
        harness.type("2k7");
        FCITX_ASSERT(harness.preedit() == "會上的");
        harness.type("review");
        harness.expect_direct_commit("會上的review ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 6. Chinese built from a tone key (及, second tone) interleaved with
    // first-tone space decisions (今 天); the full preedit is 及今天.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("ru6");
        FCITX_ASSERT(harness.preedit() == "及");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "及今");
        harness.type("wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "及今天");
    });

    // 7. Long English word edited with backspace (pops one pending char at a
    // time), then space commits the shortened word as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("github");
        FCITX_ASSERT(harness.preedit() == "github");
        harness.key(fcitx::Key("BackSpace"));
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "gith");
        harness.expect_direct_commit("gith ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 8. Chinese previews remain reversible: Backspace removes one original
    // key at a time and re-decodes the remaining raw input.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "你好");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "你cl");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "你c");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "你");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "su");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "s");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 9. Long mixed line with punctuation: Chinese 你好, pending English "ok",
    // then Shift+comma commits composition + pending word + fullwidth ，.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "你好");
        harness.type("ok");
        harness.key(fcitx::Key("Control+comma"));
        FCITX_ASSERT(harness.preedit() == "你好ok，");
        harness.expect_commit("你好ok，");
    });

    // 10. Very long composition: 你 好 我 會 上 的 下, one tone-key syllable
    // at a time; verify the preedit segment by segment, then commit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "你好");
        harness.type("ji3");
        FCITX_ASSERT(harness.preedit() == "你好我");
        harness.type("cjo4");
        FCITX_ASSERT(harness.preedit() == "你好我會");
        harness.type("g;4");
        FCITX_ASSERT(harness.preedit() == "你好我會上");
        harness.type("2k7");
        FCITX_ASSERT(harness.preedit() == "你好我會上的");
        harness.type("vu84");
        FCITX_ASSERT(harness.preedit() == "你好我會上的下");
        harness.expect_commit("你好我會上的下");
        FCITX_ASSERT(harness.preedit().empty());
    });
}
