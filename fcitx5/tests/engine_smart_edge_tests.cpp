#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// Edge cases and non-letter keys with a pending word (SmartEnglish).
// Every scenario creates its own EngineHarness and always enables
// SmartEnglish explicitly (the config is global across harnesses).
void engine_test_smart_edge(fcitx::Instance* instance) {
    // Backspace pops one pending char at a time.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hel");
        FCITX_ASSERT(harness.preedit() == "hel");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit() == "he");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit() == "h");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Return commits the pending word as English (Return is consumed).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_commit("hello");
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Escape clears the pending word.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Punctuation commits composition + pending word + the punctuation.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(fcitx::Key("Control+comma"));
        FCITX_ASSERT(harness.preedit() == "hi，");
        harness.expect_commit("hi，");
    });
    // Shift+letter commits composition + uppercase letter.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        FCITX_ASSERT(harness.preedit() == "hello");
        harness.expect_direct_commit("helloA", fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Shift+space commits composition + space.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", fcitx::Key("Shift+space"));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // ASCII punctuation remains exact while an English token is pending.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('@')));
        FCITX_ASSERT(harness.preedit() == "hello@");
        harness.expect_direct_commit("hello@ ", fcitx::Key(FcitxKey_space));
    });
    // Arrow keys settle the pending token inside the editable composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(fcitx::Key(FcitxKey_Left));
        FCITX_ASSERT(harness.preedit() == "hi");
        harness.expect_commit("hi");
    });
    // Tab does not force a client commit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(fcitx::Key(FcitxKey_Tab));
        FCITX_ASSERT(harness.preedit() == "hi");
        harness.expect_commit("hi");
    });
    // Grave settles the token, then opens the symbol menu without committing.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(fcitx::Key(FcitxKey_grave));
        FCITX_ASSERT(harness.preedit() == "hi");
        FCITX_ASSERT(harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Escape));
        harness.expect_commit("hi");
    });
    // Keypad keys commit the composition (pending word included).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_direct_commit("hi", fcitx::Key(FcitxKey_KP_Enter));
        FCITX_ASSERT(harness.preedit().empty());
    });
    // Space confirms the preview without stealing focus; Down still opens the
    // settled reading's candidate list.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
    });
}
