#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// Shift+letter types English directly (DirectlyOutputUppercase, the default),
// mirroring McBopomofo: an empty buffer passes the key through, a non-empty
// buffer commits the composition (including unfinished readings) and the
// uppercase letter in a single commit.
void engine_test_shift_letter_uppercase(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        harness.expect_direct_commit("你A", fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        // An unfinished reading is committed together with the letter.
        harness.type("gu");
        FCITX_ASSERT(harness.preedit() == "ㄕㄧ");
        harness.expect_direct_commit("ㄕㄧA", fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit().empty());
    });
}

// ShiftLetterKeys=直接放入組字區 puts the letter into the composing
// buffer in any state, without committing anything.
void engine_test_shift_letter_lowercase_buffer(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit() == "a");
        harness.expect_commit("a");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit() == "你a");
        harness.expect_commit("你a");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.set_config("CapsLockInputsBopomofo", "True");
        // With CapsLock on, the letter enters the buffer uppercase.
        harness.key(fcitx::Key(FcitxKey_a, fcitx::KeyStates(fcitx::KeyState::CapsLock) | fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit() == "A");
        harness.expect_commit("A");
    });
}

// CapsLock handling follows McBopomofo: with CapsLockInputsBopomofo=True the
// letter case is inverted (CapsLock+letter stays Chinese, Shift+CapsLock+letter
// types English); with the default False everything passes through and the
// composition is reset.
void engine_test_shift_letter_caps_lock(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("CapsLockInputsBopomofo", "True");
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::CapsLock));
        FCITX_ASSERT(harness.preedit() == "ㄇ");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("CapsLockInputsBopomofo", "True");
        harness.type("su3");
        harness.expect_direct_commit(
            "你A", fcitx::Key(FcitxKey_a, fcitx::KeyStates(fcitx::KeyState::CapsLock) | fcitx::KeyState::Shift));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("CapsLockInputsBopomofo", "False");
        harness.type("su3");
        FCITX_ASSERT(harness.preedit() == "你");
        // CapsLock on clears the composition and passes keys through to the
        // application.
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::CapsLock));
        FCITX_ASSERT(harness.preedit().empty());
    });
}

// The Hsu layout shares the same Shift behavior: tone keys only fire when
// lowercase, Shift+letter types English.
void engine_test_shift_letter_hsu(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        FCITX_ASSERT(harness.preedit() == "哦");
        harness.expect_direct_commit("哦D", fcitx::Key(FcitxKey_D, fcitx::KeyState::Shift));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_D, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit().empty());
    });
}

// Shift+space commits the composition followed by a space; with an empty
// buffer it passes through.
void engine_test_shift_space(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.type("su3");
        harness.expect_direct_commit("你 ", fcitx::Key("Shift+space"));
        FCITX_ASSERT(harness.preedit().empty());
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.key(fcitx::Key("Shift+space"));
        FCITX_ASSERT(harness.preedit().empty());
    });
}

void engine_test_shift_letter_tests(fcitx::Instance* instance) {
    engine_test_shift_letter_uppercase(instance);
    engine_test_shift_letter_lowercase_buffer(instance);
    engine_test_shift_letter_caps_lock(instance);
    engine_test_shift_letter_hsu(instance);
    engine_test_shift_space(instance);
}
