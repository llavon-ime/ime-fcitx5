#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/testing.h>
#include <fcitx/instance.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

// Test entry points provided by the engine test translation units.
void engine_test_basic_input(fcitx::Instance* instance);
void engine_test_hsu_layout_tests(fcitx::Instance* instance);
void engine_test_symbol_menu_tests(fcitx::Instance* instance);
void engine_test_candidate_navigation_tests(fcitx::Instance* instance);
void engine_test_punctuation_tests(fcitx::Instance* instance);
void engine_test_shift_letter_tests(fcitx::Instance* instance);
void engine_test_context_cache(fcitx::Instance* instance);
void engine_test_context_source(fcitx::Instance* instance);
void engine_test_async_state_tests(fcitx::Instance* instance);
void engine_test_smart_candidate(fcitx::Instance* instance);
void engine_test_smart_chat(fcitx::Instance* instance);
void engine_test_smart_config(fcitx::Instance* instance);
void engine_test_smart_decoder(fcitx::Instance* instance);
void engine_test_smart_edge(fcitx::Instance* instance);
void engine_test_smart_hsu(fcitx::Instance* instance);
void engine_test_smart_long(fcitx::Instance* instance);
void engine_test_smart_mixed(fcitx::Instance* instance);
void engine_test_smart_regressions(fcitx::Instance* instance);
void engine_test_smart_space(fcitx::Instance* instance);
void engine_test_smart_standard(fcitx::Instance* instance);
void engine_test_smart_usability(fcitx::Instance* instance);
void engine_test_smart_work(fcitx::Instance* instance);

int main() {
    fcitx::setupTestingEnvironment(TESTING_BINARY_DIR, {TESTING_BINARY_DIR},
                                   {TESTING_BINARY_DIR "/tests/test"});
    fcitx::Log::setLogRule("default=5");

    // Isolate fcitx and shared config into the build directory so the tests
    // never read or overwrite the user's real ~/.config/fcitx5 configuration.
    const auto config_home = std::filesystem::path(TESTING_BINARY_DIR) / "test-config";
    std::filesystem::create_directories(config_home);
    setenv("FCITX_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);

    // Back the accessibility context source with a file instead of the
    // desktop accessibility bus so tests never depend on a running desktop.
    const auto sample_path = config_home / "atspi-sample.txt";
    setenv("IME_FCITX5_CONTEXT_SAMPLE_FILE", sample_path.c_str(), 1);
    unsetenv("IME_FCITX5_ATSPI_SAMPLE_FILE");
    unsetenv("IME_FCITX5_DISABLE_ATSPI");
    std::filesystem::remove(sample_path);

    // Keep predictions deterministic: never connect to (or auto-start) a real
    // unix service from the engine tests. Without this, a warm service answers
    // mid-test and reorders fallback candidates.
    // Keep predictions deterministic and fast: never connect to a real unix
    // service, and never pay the auto-start retry on every prediction.
    setenv("LLAVON_IME_UNIX_SOCKET_PATH", (config_home / "no-service.sock").c_str(), 1);
    setenv("LLAVON_IME_UNIX_SERVICE_PATH", (config_home / "no-service").c_str(), 1);
    setenv("IME_FCITX5_DISABLE_SERVICE", "1", 1);

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
    engine_test_shift_letter_tests(&instance);
    engine_test_context_cache(&instance);
    engine_test_context_source(&instance);
    engine_test_async_state_tests(&instance);
    engine_test_smart_candidate(&instance);
    engine_test_smart_chat(&instance);
    engine_test_smart_config(&instance);
    engine_test_smart_decoder(&instance);
    engine_test_smart_edge(&instance);
    engine_test_smart_hsu(&instance);
    engine_test_smart_long(&instance);
    engine_test_smart_mixed(&instance);
    engine_test_smart_regressions(&instance);
    engine_test_smart_space(&instance);
    engine_test_smart_standard(&instance);
    engine_test_smart_usability(&instance);
    engine_test_smart_work(&instance);

    instance.eventDispatcher().schedule([&instance]() { instance.exit(); });
    instance.exec();

    std::printf("engine tests finished\n");
    return 0;
}
