#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// 智慧型中英文 — mixed Chinese and English inside one composition. With
// SmartEnglish on, lowercase letters typed after already-composed Chinese
// segments form an ambiguous pending word that renders raw; a tone key
// converts it to 注音, space converts a valid natural reading to Chinese or
// otherwise commits it as English.
void engine_test_smart_mixed(fcitx::Instance* instance) {
    // Chinese then English word: su3 -> 你, then "hello" is held as a pending
    // word; space commits composition + pending word + trailing space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("hello");
        FCITX_ASSERT(harness.preedit() == "你hello");
        harness.expect_direct_commit("你hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // English then Chinese: "hi" + space commits "hi ", then a fresh Chinese
    // syllable is composed normally.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        FCITX_ASSERT(harness.preedit() == "hi");
        harness.expect_direct_commit("hi ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "好");
    });

    // Two Chinese syllables then English: ji3 -> 我, cjo4 -> 會, then the
    // pending "today" is committed as English along with the composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("ji3");
        FCITX_ASSERT(harness.preedit() == "我");
        harness.type("cjo4");
        FCITX_ASSERT(harness.preedit() == "我會");
        harness.type("today");
        FCITX_ASSERT(harness.preedit() == "我會today");
        harness.expect_direct_commit("我會today ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // First-tone Chinese via space, then English: rup + space -> 今, wu0 +
    // space -> 天 (both space decisions, top candidates stay in the
    // composition), then "bye" + space commits "今天bye ".
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        FCITX_ASSERT(harness.preedit() == "rup");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        FCITX_ASSERT(harness.preedit() == "今wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今天");
        harness.type("bye");
        FCITX_ASSERT(harness.preedit() == "今天bye");
        harness.expect_direct_commit("今天bye ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // English word in the middle of Chinese typing: the pending "so" commits
    // as English (not a valid natural reading), leaving 你 committed; the
    // following syllable composes fresh.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("so");
        FCITX_ASSERT(harness.preedit() == "你so");
        harness.expect_direct_commit("你so ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "好");
    });

    // Tone decision converts only the pending word: ru6 -> 及, then the
    // pending wu0 converts to 天 on space while the earlier 及 stays.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("ru6");
        FCITX_ASSERT(harness.preedit() == "及");
        harness.type("wu0");
        FCITX_ASSERT(harness.preedit() == "及wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "及天");
    });

    // English word + punctuation after Chinese: cl3 -> 好, pending "ok" is
    // committed together with the fullwidth period in one commit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "好");
        harness.type("ok");
        FCITX_ASSERT(harness.preedit() == "好ok");
        harness.key(fcitx::Key("Control+period"));
        FCITX_ASSERT(harness.preedit() == "好ok。");
        harness.expect_commit("好ok。");
    });

    // Chinese + English + Chinese: su3 -> 你, "hi" + space commits "你hi ",
    // then vu84 composes 下 in the fresh buffer.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("hi");
        FCITX_ASSERT(harness.preedit() == "你hi");
        harness.expect_direct_commit("你hi ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("vu84");
        FCITX_ASSERT(harness.preedit() == "下");
    });

    // Reset the global config so later test files run with the default OFF.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
    });
}
