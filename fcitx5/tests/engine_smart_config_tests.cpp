#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// The SmartEnglish config option: default OFF preserves existing behavior
// exactly; turning it ON holds pending letters raw until a tone key or space
// decides; it can be toggled at any time. The config is GLOBAL across
// scenarios, so every scenario sets the config it needs explicitly.
void engine_test_smart_config(fcitx::Instance* instance) {
    // 1. Default OFF: letters are 注音 immediately. "su" renders as ㄋㄧ (not
    //    raw "su"), then the tone key 3 (ˇ) gives 你.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "ㄋㄧ");
        harness.type("3");
        FCITX_ASSERT(harness.preedit() == "你");
    });
    // 2. OFF: space with a completed syllable opens the candidate list.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.has_candidates());
    });
    // 3. ON: letters are held raw in the preedit; the tone key replays them
    //    as 注音 -> 你.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "su");
        harness.type("3");
        FCITX_ASSERT(harness.preedit() == "你");
    });
    // 4. Toggle in the same run: ON types English (hello + space -> "hello "),
    //    then a fresh harness sets OFF again and 注音 behavior is restored.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "ㄋㄧ");
    });
    // 5. OFF: English letters go through the bopomofo path. "hello" has no
    //    valid table reading, so the space decision drops the segment and the
    //    preedit ends up empty.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("hello");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // 6. Config isolation: explicit set each time. OFF -> 注音, ON -> raw +
    //    tone, OFF -> 注音 again, in three fresh harnesses.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "ㄋㄧ");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "su");
        harness.type("3");
        FCITX_ASSERT(harness.preedit() == "你");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        FCITX_ASSERT(harness.preedit() == "ㄋㄧ");
    });
    // 7. ON with the Hsu layout: known "hd" stays raw until Down exposes 哦.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("h");
        FCITX_ASSERT(harness.preedit() == "h");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('d')));
        FCITX_ASSERT(harness.preedit() == "hd");
        harness.key(fcitx::Key(FcitxKey_Down));
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "哦");
    });
    // 8. OFF with the Hsu layout keeps existing behavior: "hd" resolves
    //    directly to 哦.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        FCITX_ASSERT(harness.preedit() == "哦");
    });
}
