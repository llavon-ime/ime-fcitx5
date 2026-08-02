#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

void engine_test_hsu_input(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");

        harness.type("cen ");
        FCITX_ASSERT(harness.preedit() == "心");

        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "心");

        harness.key(fcitx::Key(FcitxKey_1));
        harness.expect_commit("心");
    });
}

void engine_test_hsu_halfwidth_punctuation(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_comma));
        FCITX_ASSERT(harness.preedit() == ",");
        harness.expect_commit(",");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key("Shift+comma"));
        FCITX_ASSERT(harness.preedit() == ",");
        harness.expect_commit(",");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_bracketleft));
        FCITX_ASSERT(harness.preedit() == "[");
        harness.expect_commit("[");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_semicolon));
        FCITX_ASSERT(harness.preedit() == ";");
        harness.expect_commit(";");
    });
}

void engine_test_hsu_fullwidth_punctuation(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_less));
        FCITX_ASSERT(harness.preedit() == "，");
        harness.expect_commit("，");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_braceleft));
        FCITX_ASSERT(harness.preedit() == "『");
        harness.expect_commit("『");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(fcitx::Key(FcitxKey_colon));
        FCITX_ASSERT(harness.preedit() == "：");
        harness.expect_commit("：");
    });
}

void engine_test_hsu_tones(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        FCITX_ASSERT(harness.preedit() == "哦");
        harness.expect_commit("哦");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("geej");
        FCITX_ASSERT(harness.preedit() == "界");
        harness.expect_commit("界");
    });
}

void engine_test_hsu_digits(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.expect_commit("5");
        harness.type("5");
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("cen ");
        harness.type("5");
        FCITX_ASSERT(harness.preedit() == "心5");
        harness.expect_commit("心5");
    });
}

void engine_test_hsu_alternative_reading(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("a ");
        FCITX_ASSERT(harness.preedit() == "疵");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "疵");
        bool saw_alternative = false;
        for (size_t i = 0; i < harness.candidate_count(); ++i) {
            if (harness.candidate(i) == "ㄟ") saw_alternative = true;
        }
        FCITX_ASSERT(saw_alternative);

        harness.key(fcitx::Key(FcitxKey_1));
        harness.expect_commit("疵");
    });
}

void engine_test_hsu_layout_tests(fcitx::Instance* instance) {
    engine_test_hsu_input(instance);
    engine_test_hsu_halfwidth_punctuation(instance);
    engine_test_hsu_fullwidth_punctuation(instance);
    engine_test_hsu_tones(instance);
    engine_test_hsu_digits(instance);
    engine_test_hsu_alternative_reading(instance);
}
