#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// SmartEnglish on the Hsu (許氏) layout: lowercase letters are held as a raw
// pending word; the Hsu tone keys d/f/j/s decide Chinese (replay as 注音),
// space decides Chinese (first-tone reading) or English (word + space).
void engine_test_smart_hsu(fcitx::Instance* instance) {
    // 1. A known Latin token stays raw; Down exposes its Hsu interpretation.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("h");
        FCITX_ASSERT(harness.preedit() == "h");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('d')));
        FCITX_ASSERT(harness.preedit() == "hd");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "hd");
        FCITX_ASSERT(harness.candidate(1) == "哦");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "哦");
    });
    // 2. Pending letters render raw until the tone key: "ne" is NOT ㄋㄧ.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("ne");
        FCITX_ASSERT(harness.preedit() == "ne");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('f')));
        FCITX_ASSERT(harness.preedit() == "你");
    });
    // 3. Tone keys f/j/s decide Chinese: hw+f -> 好, xh+f -> 我.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hw");
        FCITX_ASSERT(harness.preedit() == "hw");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('f')));
        FCITX_ASSERT(harness.preedit() == "好");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("xh");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('f')));
        FCITX_ASSERT(harness.preedit() == "我");
    });
    // 4. Hsu first tone via space: gen + space -> 今 (ㄐㄧㄣ first tone).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("gen");
        FCITX_ASSERT(harness.preedit() == "gen");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "今");
    });
    // 5. Hsu English via space: word plus a trailing space. (Note: "hi" maps
    //    to the natural Hsu reading ㄏㄞ (嗨), so English words here must not
    //    form natural Hsu readings.)
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("thank");
        harness.expect_direct_commit("thank ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // 6. Hsu mixed: 你 via nef, then English hello + space commits 你hello .
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("nef");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.type("hello");
        harness.expect_direct_commit("你hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // 7. A Hsu tone-looking letter remains part of the English token when the
    //    combined sequence is not a valid reading.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('d')));
        FCITX_ASSERT(harness.preedit() == "hellod");
        harness.expect_direct_commit("hellod ", fcitx::Key(FcitxKey_space));
    });
    // 8. Backspace pops one pending char at a time.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("ne");
        FCITX_ASSERT(harness.preedit() == "ne");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit() == "n");
        harness.key(fcitx::Key("BackSpace"));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // 9. A tone key with nothing pending starts a pending word.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('d')));
        FCITX_ASSERT(harness.preedit() == "d");
    });
    // 10. Hsu Chinese then English: hd -> 哦, then hello + space -> 哦hello .
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hd");
        harness.key(fcitx::Key(FcitxKey_Down));
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "哦");
        harness.type("hello");
        harness.expect_direct_commit("哦hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
}
