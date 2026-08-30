#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

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

    // A plain reset (e.g. the engine clearing its composition, IC destroyed)
    // must NOT clear the cache: only a FocusOut transition resets the
    // history so text does not leak between fields.
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

    // Disabling the cache keeps commits from being recorded.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"ContextHistoryLimit", "0"}});
        harness.type("su3");
        harness.expect_commit("你");
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(!cache.valid());
    });

    // With backspace tracking enabled, a Backspace outside the composition
    // pops the cache so the recorded history follows the document.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"TrackContextBackspace", "True"}});
        harness.type("su3");
        harness.expect_commit("你");
        harness.type("cl3");
        harness.expect_commit("好");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        const auto cache = harness.engine_state()->context_cache;
        FCITX_ASSERT(cache.valid());
        FCITX_ASSERT(cache.window(100) == std::u16string(u"你"));
    });
}
