#include "engine_harness.hpp"

#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

void engine_test_punctuation_tests(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);

        harness.type("[");
        harness.expect_commit("「");

        harness.type("]");
        harness.expect_commit("」");

        harness.key(fcitx::Key(FcitxKey_question));
        harness.expect_commit("？");

        harness.key(fcitx::Key(FcitxKey_braceleft));
        harness.expect_commit("『");

        harness.key(fcitx::Key(FcitxKey_braceright));
        harness.expect_commit("』");

        harness.key(fcitx::Key(FcitxKey_colon));
        harness.expect_commit("：");

        harness.key(fcitx::Key(FcitxKey_quotedbl));
        harness.expect_commit("；");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.type(",");
        FCITX_ASSERT(harness.preedit() == "ㄝ");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.type("su3");
        harness.type("[");
        harness.expect_commit("你「");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.key(fcitx::Key("Control+exclam"));
        harness.expect_commit("！");

        harness.key(fcitx::Key("Control+apostrophe"));
        harness.expect_commit("、");

        harness.key(fcitx::Key("Control+comma"));
        harness.expect_commit("，");

        harness.key(fcitx::Key("Control+period"));
        harness.expect_commit("。");

        harness.key(fcitx::Key("Control+slash"));
        harness.expect_commit("？");

        harness.key(fcitx::Key("Control+semicolon"));
        harness.expect_commit("；");
    });

    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");

        harness.type(",");
        harness.expect_commit(",");

        harness.key(fcitx::Key(FcitxKey_less));
        harness.expect_commit("，");

        harness.type("[");
        harness.expect_commit("[");

        harness.key(fcitx::Key(FcitxKey_braceleft));
        harness.expect_commit("『");

        harness.type(";");
        harness.expect_commit(";");

        harness.key(fcitx::Key(FcitxKey_colon));
        harness.expect_commit("：");

        harness.type("'");
        harness.expect_commit("'");

        harness.key(fcitx::Key(FcitxKey_quotedbl));
        harness.expect_commit("；");

        harness.type("b");
        FCITX_ASSERT(harness.preedit() == "ㄅ");
    });
}
