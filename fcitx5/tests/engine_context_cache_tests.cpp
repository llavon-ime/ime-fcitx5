#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>
#include <utility>

using namespace ime::fcitx5::test;

namespace {

void seed_cache(EngineHarness& harness, std::u16string text) {
    harness.engine_state()->context_cache.on_commit(std::move(text));
}

}  // namespace

// Self-managed context cache: the engine remembers what it committed so the
// prediction request can carry context even when the client never pushes
// surrounding text, and it resynchronizes when the client does.
void engine_test_context_cache(fcitx::Instance* instance) {
    // Committed Chinese + English accumulate in the cache.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        FCITX_ASSERT(harness.engine_state() != nullptr);
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.valid());
        FCITX_ASSERT(cache.window(100) == std::u16string(u"你好"));
    });

    // The window is capped by the configured limit.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        for (int i = 0; i < 30; ++i) {
            harness.type("su3");
            harness.expect_commit("你");
        }
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.window(10000).size() == 30);
    });

    // Surrounding text resyncs the cache: a client window that ends with our
    // history extends it with the older text.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.set_surrounding("hello world 你", 13, 13);
        // A completed syllable triggers a prediction request, which resyncs
        // the cache against the surrounding text first.
        harness.type("su3");
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.window(100) == std::u16string(u"hello world 你"));
    });

    // A mismatched surrounding window (caret moved / external edit) replaces
    // the cache instead of serving stale text.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.set_surrounding("completely different", 10, 10);
        harness.type("su3");
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.window(100) == std::u16string(u"completely"));
    });

    // Selection endpoints are scalar offsets, including supplementary text.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"ContextHistoryLimit", "4"},
                             {"ContextLength", "2"}});
        for (const auto& endpoints : {std::pair<size_t, size_t>{3, 5}, {5, 3}}) {
            harness.set_surrounding("a\xF0\x9F\x98\x80" "bcd", endpoints.first, endpoints.second);
            harness.type("su3");
            FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"a\U0001F600b");
            harness.input_context()->reset();
        }
        harness.set_config("ContextHistoryLimit", "0");
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"b");
        harness.set_configs({{"ContextHistoryLimit", "1024"}, {"ContextLength", "512"}});
    });

    // A plain reset must not clear history.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.input_context()->reset();
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.valid());
        FCITX_ASSERT(cache.window(100) == std::u16string(u"你"));
    });

    // FocusOut clears history by default.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ResetContextOnFocusOut", "True");
        seed_cache(harness, u"你");
        harness.input_context()->focusOut();
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // The explicit opt-out preserves history across FocusOut.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ResetContextOnFocusOut", "False");
        seed_cache(harness, u"你");
        harness.input_context()->focusOut();
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == std::u16string(u"你"));
        harness.set_config("ResetContextOnFocusOut", "True");
    });

    // Entering a sensitive field clears previously recorded context.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        auto* context = harness.input_context();
        auto capabilities = context->capabilityFlags();
        capabilities |= fcitx::CapabilityFlag::PasswordOrSensitive;
        context->setCapabilityFlags(capabilities);
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // Disabling self-managed history still permits client surrounding text.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"ContextHistoryLimit", "0"}});
        harness.type("su3");
        harness.expect_commit("你");
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
        harness.set_surrounding("hello", 5, 5);
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == std::u16string(u"hello"));
        harness.set_config("ContextHistoryLimit", "1024");
    });

    // Changing the history limit also updates contexts that are not currently
    // active, so disabling and re-enabling it cannot revive old history.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("ResetContextOnFocusOut", "False");
        seed_cache(harness, u"secret");
        harness.input_context()->focusOut();
        EngineHarness active(instance);
        active.set_config("ContextHistoryLimit", "3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"ret");
        active.set_config("ContextHistoryLimit", "0");
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
        harness.engine_state()->context_cache.on_surrounding(u"abcdef", 6);
        active.set_config("ContextLength", "2");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"ef");
        active.set_configs({{"ContextHistoryLimit", "1024"}, {"ContextLength", "512"},
                            {"ResetContextOnFocusOut", "True"}});
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"ef");
    });

    // Modified Backspace can delete more than one character and invalidates.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你好");
        harness.key(fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Ctrl));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你好");
        harness.key(fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Alt));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你好");
        harness.key(fcitx::Key(FcitxKey_Delete));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // With edit tracking on by default, a Backspace outside the composition
    // clears the cache rather than guessing the client's deletion semantics.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
        FCITX_ASSERT(cache.window(100).empty());
    });

    // Shift+Backspace is equally ambiguous.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        harness.key(fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Shift));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        for (const auto* text : {u"prefix e\u0301", u"prefix \U0001F469\u200D\U0001F4BB"}) {
            seed_cache(harness, text);
            FCITX_ASSERT(!harness.key_accepted(fcitx::Key(FcitxKey_BackSpace)));
            FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
        }
        harness.set_surrounding("selected text", 0, 8);
        seed_cache(harness, u"selected text");
        FCITX_ASSERT(!harness.key_accepted(fcitx::Key(FcitxKey_BackSpace)));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        seed_cache(harness, u"prefix");
        harness.type("s");
        FCITX_ASSERT(harness.key_accepted(fcitx::Key(FcitxKey_BackSpace)));
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"prefix");
    });

    // A caret jump (Up/Down/Home/End/Page) with an empty composition clears
    // the cache: the recorded text is no longer before the caret.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        harness.key(fcitx::Key(FcitxKey_Up));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
        FCITX_ASSERT(cache.window(100).empty());
    });

    // Undo / cut / select-all clear the cache too.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.key(fcitx::Key(FcitxKey_z, fcitx::KeyState::Ctrl));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
    });

    // A caret move makes the text-before-caret cache ambiguous.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.key(fcitx::Key(FcitxKey_Left));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
    });

    // Paste shortcuts introduce text the engine cannot observe.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你");
        harness.key(fcitx::Key(FcitxKey_v, fcitx::KeyState::Ctrl));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // Pass-through edits under CapsLock and Shift+Space invalidate history
    // because the application changes text outside the engine's view.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("CapsLockInputsBopomofo", "False");
        seed_cache(harness, u"你");
        harness.key(fcitx::Key(FcitxKey_A, fcitx::KeyState::CapsLock));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你");
        harness.key(fcitx::Key("Shift+space"));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你");
        harness.key(fcitx::Key(FcitxKey_Insert, fcitx::KeyState::Shift));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // macOS Command is delivered as a Super/Meta-style modifier rather than
    // Ctrl, but these shortcuts still modify the application's document.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        seed_cache(harness, u"你");
        harness.key(fcitx::Key(FcitxKey_v, fcitx::KeyState::Super));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
        seed_cache(harness, u"prefix");
        harness.key(fcitx::Key(FcitxKey_v, fcitx::KeyState::Meta));
        FCITX_ASSERT(!harness.engine_state()->context_cache.valid());
    });

    // Navigation inside a non-empty composition must NOT clear the cache
    // (the engine handles those keys itself and the document is untouched).
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.key(fcitx::Key(FcitxKey_Up));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.valid());
        FCITX_ASSERT(cache.window(100) == std::u16string(u"你"));
    });

    // The retention cap bounds the recorded history.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"ContextHistoryLimit", "2"}});
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        harness.type("ji3");
        harness.expect_commit("我");
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.valid());
        FCITX_ASSERT(cache.window(100) == std::u16string(u"好我"));
    });
}
