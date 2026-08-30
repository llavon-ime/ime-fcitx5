#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// 聊天情境: realistic chat messages mixing Chinese and English with
// SmartEnglish on. Pending English words are held raw in the preedit; space
// decides English (commit word + trailing space) or Chinese (first-tone
// reading), tone keys decide Chinese.
void engine_test_smart_chat(fcitx::Instance* instance) {
    // 1. English greeting: raw letters held, space commits word + space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        FCITX_ASSERT(harness.preedit() == "hello");
        harness.expect_direct_commit("hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 2. Chinese reply: two tone-key syllables compose into one preedit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "你好");
    });

    // 3. English goodbye.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("bye");
        harness.expect_direct_commit("bye ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 4. Mixed one-line chat: Chinese syllable, then English pending word;
    // space commits composition + pending word + trailing space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("good");
        FCITX_ASSERT(harness.preedit() == "你good");
        harness.expect_direct_commit("你good ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 5. First-tone Chinese in chat: space turns a natural reading into
    // Chinese (今), and the next first-tone syllable appends (今天).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今天");
    });

    // 6. Chat English after Chinese: pending word commits with the
    // composition and a trailing space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.type("hello");
        FCITX_ASSERT(harness.preedit() == "今hello");
        harness.expect_direct_commit("今hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 7. English with punctuation: Shift+comma commits word + fullwidth ，.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(fcitx::Key("Control+comma"));
        FCITX_ASSERT(harness.preedit() == "hi，");
        harness.expect_commit("hi，");
    });

    // 8. Chat particle: tone key 7 (˙) turns the pending word into 的.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("2k7");
        FCITX_ASSERT(harness.preedit() == "的");
    });

    // 9. Toggle-style chat words, each in its own harness.
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
        harness.type("yes");
        harness.expect_direct_commit("yes ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
}
