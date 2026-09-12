#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ime::fcitx5::test;

// The engine prefers the AT-SPI caret sample over its own commit history when
// the client never pushes surrounding text. The engine tests run with
// IME_FCITX5_ATSPI_SAMPLE_FILE, which backs the provider with a file instead of
// the accessibility bus: writing the file publishes a sample.
void engine_test_context_source(fcitx::Instance* instance) {
    const char* sample_path = std::getenv("IME_FCITX5_ATSPI_SAMPLE_FILE");
    FCITX_ASSERT(sample_path != nullptr && sample_path[0] != '\0');
    const std::filesystem::path path(sample_path);

    // Text this IME never committed becomes the prediction context.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "早安，今天天氣很好。";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"UseAccessibilityContext", "True"}});
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"早安，今天天氣很好。");
        std::filesystem::remove(path);
    });

    // An unusable sample (missing file) leaves the self-managed history alone.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"UseAccessibilityContext", "True"}});
        harness.type("su3");
        harness.expect_commit("你");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"你");
    });

    // Disabling the source stops using it even when a sample is available.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "不應該被使用";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}, {"UseAccessibilityContext", "False"}});
        harness.type("su3");
        harness.expect_commit("你");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"你");
        std::filesystem::remove(path);
        harness.set_config("UseAccessibilityContext", "True");
    });
}
