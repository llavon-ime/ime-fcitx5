#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// Shift+letter types English directly (DirectlyOutputUppercase, the default),
// mirroring McBopomofo: an empty buffer passes the key through, a non-empty
// buffer commits the composition (including unfinished readings) and the
// uppercase letter in a single commit.
RAWKEY_SUITE("shift letter uppercase", shift_letter_uppercase) {
    {
        Harness harness;
        harness.key(Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
    {
        Harness harness;
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_direct_commit("你A", Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
    {
        Harness harness;
        // An unfinished reading is committed together with the letter.
        harness.type("gu");
        RAWKEY_ASSERT(harness.preedit() == "ㄕㄧ");
        harness.expect_direct_commit("ㄕㄧA", Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
}

// ShiftLetterKeys=直接放入組字區 puts the letter into the composing buffer in
// any state, without committing anything.
RAWKEY_SUITE("shift letter buffer", shift_letter_lowercase_buffer) {
    {
        Harness harness;
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.key(Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit() == "a");
        harness.expect_commit("a");
    }
    {
        Harness harness;
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.type("su3");
        harness.key(Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit() == "你a");
        harness.expect_commit("你a");
    }
    {
        Harness harness;
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.set_config("CapsLockInputsBopomofo", "True");
        // The frontends resolve Shift XOR CapsLock into the keysym case before
        // the engine sees the key, so CapsLock alone arrives as lowercase.
        harness.key(Key("a").with(kCapsLock));
        RAWKEY_ASSERT(harness.preedit() == "A");
        harness.expect_commit("A");
    }
}

// CapsLock handling follows McBopomofo: with CapsLockInputsBopomofo=True the
// letter case is inverted (CapsLock+letter stays Chinese,
// Shift+CapsLock+letter types English); with the default False everything
// passes through and the composition is reset.
RAWKEY_SUITE("shift letter caps lock", shift_letter_caps_lock) {
    {
        Harness harness;
        harness.set_config("CapsLockInputsBopomofo", "True");
        harness.key(Key("A").with(kCapsLock));
        RAWKEY_ASSERT(harness.preedit() == "ㄇ");
    }
    {
        Harness harness;
        harness.set_config("CapsLockInputsBopomofo", "True");
        harness.type("su3");
        // CapsLock+letter types English because the keysym stays lowercase.
        harness.expect_direct_commit("你A", Key("a").with(kCapsLock));
    }
    {
        Harness harness;
        harness.set_config("CapsLockInputsBopomofo", "False");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        // CapsLock on clears the composition and passes keys through.
        harness.key(Key("A").with(kCapsLock));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
}

// The Hsu layout shares the same Shift behavior: tone keys only fire when
// lowercase, Shift+letter types English.
RAWKEY_SUITE("shift letter hsu", shift_letter_hsu) {
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        RAWKEY_ASSERT(harness.preedit() == "哦");
        harness.expect_direct_commit("哦D", Key("D").with(kShift));
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(Key("D").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
}

// Shift+space commits the composition followed by a space; with an empty
// buffer it passes through.
RAWKEY_SUITE("shift space", shift_space) {
    {
        Harness harness;
        harness.type("su3");
        harness.expect_direct_commit("你 ", Key("space").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
    {
        Harness harness;
        harness.key(Key("space").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
    }
}
