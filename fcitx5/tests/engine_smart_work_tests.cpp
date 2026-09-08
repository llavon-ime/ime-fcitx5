#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// SmartEnglish (智慧型中英文) work/office scenarios: meeting notes, status
// reports and technical chat. With SmartEnglish=ON, lowercase pending chars
// are held ambiguously; a tone key resolves them as 注音 while space resolves
// a valid reading as Chinese (first tone) or otherwise commits English.
void engine_test_smart_work(fcitx::Instance* instance) {
    // 1. Meeting word: "meeting" + space commits as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        harness.expect_direct_commit("meeting ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 2. Report status: 報 (1l4) + 告 (el4) compose 報告, Return commits it.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("1l4");
        FCITX_ASSERT(harness.preedit() == "報");
        harness.type("el4");
        FCITX_ASSERT(harness.preedit() == "報告");
        harness.expect_commit("報告");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 3. Deadline word: "deadline" + space commits as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("deadline");
        harness.expect_direct_commit("deadline ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 4. Review request: "review" + space commits as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("review");
        harness.expect_direct_commit("review ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 5. Chinese sentence: 會 (cjo4) 上 (g;4) 的 (2k7) combine to 會上的.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("cjo4");
        FCITX_ASSERT(harness.preedit() == "會");
        harness.type("g;4");
        FCITX_ASSERT(harness.preedit() == "會上");
        harness.type("2k7");
        FCITX_ASSERT(harness.preedit() == "會上的");
        harness.expect_commit("會上的");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 6. Mixed status line: 你 (su3) followed by English "good" + space
    // commits both: "你good ".
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

    // 7. Bug report: "bug" + space commits English, then 的 (2k7) resumes
    // Chinese composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("bug");
        harness.expect_direct_commit("bug ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("2k7");
        FCITX_ASSERT(harness.preedit() == "的");
        harness.expect_commit("的");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 8. First-tone work word: 說 (gxi + space, Chinese first-tone decision),
    // then 及 (ru6, second tone via tone key) appends to the composition.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("gji");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit() == "說");
        harness.type("ru6");
        FCITX_ASSERT(harness.preedit() == "說及");
        harness.expect_commit("說及");
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 9. GitHub: "github" + space commits as English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("github");
        harness.expect_direct_commit("github ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });

    // 10. Test result: "test" + space then "ok" + space, both English.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("test");
        harness.expect_direct_commit("test ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("ok");
        harness.expect_direct_commit("ok ", fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.preedit().empty());
    });
}
