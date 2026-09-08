#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

// Mixed-input decoder scenarios: structured ASCII followed by Chinese keeps a
// complete mixed candidate, while Email/URL/path/code/number input is never
// silently rewritten. The exact raw ASCII path always remains selectable.
void engine_test_smart_decoder(fcitx::Instance* instance) {
    // URL + Chinese: the complete mixed candidate commits both parts.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com5j/");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "https://example.com5j/");
        FCITX_ASSERT(harness.candidate(1) == "https://example.com中");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "https://example.com中");
        harness.expect_commit("https://example.com中");
    });

    // Filesystem path + Chinese.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("/tmp/283");
        FCITX_ASSERT(harness.preedit() == "/tmp/283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate(0) == "/tmp/283");
        FCITX_ASSERT(harness.candidate(1) == "/tmp/打");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "/tmp/打");
        harness.expect_commit("/tmp/打");
    });

    // Identifier + Chinese.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("hello_world283");
        FCITX_ASSERT(harness.preedit() == "hello_world283");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "hello_world283");
        FCITX_ASSERT(harness.candidate(1) == "hello_world打");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("hello_world打");
    });

    // Versions, IPs, ports, dates and decimals commit as exact ASCII.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("v1.2.3");
        harness.expect_direct_commit("v1.2.3 ", fcitx::Key(FcitxKey_space));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("192.168.1.1");
        harness.expect_direct_commit("192.168.1.1 ", fcitx::Key(FcitxKey_space));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("localhost:8080");
        harness.expect_direct_commit("localhost:8080 ", fcitx::Key(FcitxKey_space));
    });
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("2026-08-08");
        harness.expect_direct_commit("2026-08-08 ", fcitx::Key(FcitxKey_space));
    });

    // mp3 is a known English token and does not open candidates automatically.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("mp3");
        FCITX_ASSERT(!harness.has_candidates());
        harness.expect_direct_commit("mp3 ", fcitx::Key(FcitxKey_space));
    });

    // Backspace removes an explicitly applied Chinese candidate, then the next
    // numeric token remains raw until Chinese is requested again.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("283");
        FCITX_ASSERT(harness.preedit() == "283");
        harness.key(fcitx::Key(FcitxKey_Down));
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "打");
        harness.key(fcitx::Key(FcitxKey_BackSpace));
        FCITX_ASSERT(harness.preedit().empty());
        harness.type("284");
        FCITX_ASSERT(harness.preedit() == "284");
        harness.key(fcitx::Key(FcitxKey_Down));
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "大");
        harness.expect_commit("大");
    });

    // Hsu: a tone-looking key inside an English token re-decodes without
    // consuming letters as candidates.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("hellod");
        FCITX_ASSERT(harness.preedit() == "hellod");
        harness.expect_direct_commit("hellod ", fcitx::Key(FcitxKey_space));
    });

    // Hsu: Chinese sequences after a domain prefix keep the mixed candidate.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("mail.google.comnef");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "mail.google.comnef");
        FCITX_ASSERT(harness.candidate(1) == "mail.google.co敏");
        harness.key(fcitx::Key(FcitxKey_2));
        harness.expect_commit("mail.google.co敏");
    });

    // Every complete path shown by the decoder must be selectable, including
    // paths containing more than one Bopomofo segment.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello283su3");
        FCITX_ASSERT(!harness.has_candidates());
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.candidate(0) == "hello283su3");
        FCITX_ASSERT(harness.candidate(1) == "hello打你");
        harness.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(harness.preedit() == "hello打你");
        harness.expect_commit("hello打你");
    });

    // FocusOut commits the captured raw input exactly once.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com/v1.2.3");
        harness.expect_focus_out_commit("https://example.com/v1.2.3");
        harness.input_context()->focusIn();
    });
}
