#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// SmartEnglish on the standard keyboard: lowercase pending characters render
// as raw ASCII in the preedit until a tone key (3/6/4/7) or space decides
// Chinese vs English. All sequences below are verified against
// bopomofo_char.json.
void engine_test_smart_standard(fcitx::Instance* instance) {
    // Pending letters stay raw before the tone decision.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "su");
        harness.key(fcitx::Key(FcitxKey_3));
        FCITX_ASSERT(harness.preedit() == "你");
    });
    // Each standard tone key converts ㄋㄧ (s, u) to its top candidate.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(fcitx::Key(FcitxKey_3));
        FCITX_ASSERT(harness.preedit() == "你");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(fcitx::Key(FcitxKey_6));
        FCITX_ASSERT(harness.preedit() == "泥");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(fcitx::Key(FcitxKey_4));
        FCITX_ASSERT(harness.preedit() == "逆");
    });
    // ㄋㄧ˙ is not a reading in the table: keep the whole raw token editable.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(fcitx::Key(FcitxKey_7));
        FCITX_ASSERT(harness.preedit() == "su7");
        harness.expect_direct_commit("su7 ", fcitx::Key(FcitxKey_space));
    });
    // Second tone: ru (ㄐㄧ) + 6 -> 及 (ㄐㄧˊ top candidate).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("ru");
        harness.key(fcitx::Key(FcitxKey_6));
        FCITX_ASSERT(harness.preedit() == "及");
    });
    // Neutral tone: 2k (ㄉㄜ) + 7 -> 的.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("2k");
        harness.key(fcitx::Key(FcitxKey_7));
        FCITX_ASSERT(harness.preedit() == "的");
    });
    // A tone-looking digit in an English token remains editable ASCII.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(fcitx::Key(FcitxKey_6));
        FCITX_ASSERT(harness.preedit() == "hello6");
        harness.expect_direct_commit("hello6 ", fcitx::Key(FcitxKey_space));
    });
    // Multi-syllable: consecutive tone-key conversions append.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        FCITX_ASSERT(harness.preedit() == "好");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "好你");
    });
    // Pending chars include the bopomofo digits: 1l (ㄅㄠ) + 4 -> 報.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("1l");
        FCITX_ASSERT(harness.preedit() == "1l");
        harness.key(fcitx::Key(FcitxKey_4));
        FCITX_ASSERT(harness.preedit() == "報");
    });
    // Bopomofo finals entered with , . ; are pending chars too.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("g;");
        FCITX_ASSERT(harness.preedit() == "g;");
        harness.key(fcitx::Key(FcitxKey_4));
        FCITX_ASSERT(harness.preedit() == "上");
    });
    // Space decides Chinese (first tone) for a valid two-char reading: 周.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("5.");
        FCITX_ASSERT(harness.preedit() == "5.");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "周");
    });
    // After an English space-commit, a fresh pending word still converts.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_direct_commit("hi ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
    });
    // Full syllable vu8 (ㄒㄧㄚ) + 4 -> 下.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("vu8");
        FCITX_ASSERT(harness.preedit() == "vu8");
        harness.key(fcitx::Key(FcitxKey_4));
        FCITX_ASSERT(harness.preedit() == "下");
    });
    // wu0 (ㄊㄧㄢ) + space -> 天 (first-tone space decision).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("wu0");
        FCITX_ASSERT(harness.preedit() == "wu0");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "天");
    });
}
