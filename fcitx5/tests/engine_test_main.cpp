#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/testing.h>
#include <fcitx/instance.h>

#include <cstdio>

// Test entry points provided by the engine test translation units.
void engine_test_basic_input(fcitx::Instance* instance);
void engine_test_hsu_layout_tests(fcitx::Instance* instance);
void engine_test_symbol_menu_tests(fcitx::Instance* instance);
void engine_test_candidate_navigation_tests(fcitx::Instance* instance);
void engine_test_punctuation_tests(fcitx::Instance* instance);

int main() {
    fcitx::setupTestingEnvironment(TESTING_BINARY_DIR, {TESTING_BINARY_DIR},
                                   {TESTING_BINARY_DIR "/tests/test"});
    fcitx::Log::setLogRule("default=5");

    char arg0[] = "test-engine";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testim,testfrontend,llavon-ime";
    char* argv[] = {arg0, arg1, arg2};
    fcitx::Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance.addonManager().registerDefaultLoader(nullptr);

    engine_test_basic_input(&instance);
    engine_test_hsu_layout_tests(&instance);
    engine_test_symbol_menu_tests(&instance);
    engine_test_candidate_navigation_tests(&instance);
    engine_test_punctuation_tests(&instance);

    instance.eventDispatcher().schedule([&instance]() { instance.exit(); });
    instance.exec();

    std::printf("engine tests finished\n");
    return 0;
}
