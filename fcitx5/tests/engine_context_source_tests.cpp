#include "engine_harness.hpp"
#include "fcitx5/ime_config.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace ime::fcitx5::test;

namespace {

const ime::fcitx5::ImeFcitxConfig* engine_config(fcitx::Instance* instance) {
    auto* addon = instance->addonManager().addon("llavon-ime");
    if (addon == nullptr) return nullptr;
    return static_cast<const ime::fcitx5::ImeFcitxConfig*>(addon->getConfig());
}

std::string accessibility_status(fcitx::Instance* instance) {
    const auto* config = engine_config(instance);
    return config != nullptr ? *config->accessibilityStatus : std::string();
}

bool status_contains(const std::string& status, std::string_view needle) {
    return status.find(needle) != std::string::npos;
}

}  // namespace

// The engine prefers the AT-SPI caret sample over its own commit history when
// the client never pushes surrounding text. The engine tests run with
// IME_FCITX5_CONTEXT_SAMPLE_FILE, which backs the provider with a file instead of
// the accessibility bus: writing the file publishes a sample.
void engine_test_context_source(fcitx::Instance* instance) {
    const char* sample_path = std::getenv("IME_FCITX5_CONTEXT_SAMPLE_FILE");
    FCITX_ASSERT(sample_path != nullptr && sample_path[0] != '\0');
    const std::filesystem::path path(sample_path);

    // Text this IME never committed becomes the prediction context.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "早安，今天天氣很好。";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"早安，今天天氣很好。");
        std::filesystem::remove(path);
    });

    // The config UI shows whether accessibility context can be obtained, and
    // opening the configuration (which reloads it) must not clear that status.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        FCITX_ASSERT(engine_config(instance) != nullptr);
        FCITX_ASSERT(status_contains(accessibility_status(instance), "可取得"));
        auto* addon = instance->addonManager().addon("llavon-ime");
        addon->reloadConfig();
        FCITX_ASSERT(status_contains(accessibility_status(instance), "可取得"));
        fcitx::RawConfig shown;
        addon->getConfig()->save(shown);
        const auto* value = shown.valueByPath("AccessibilityStatus");
        FCITX_ASSERT(value != nullptr && !value->empty());
    });

    // Disabling self-managed history must not disable the accessibility
    // source: the sample is adopted through the surrounding-text budget.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "歷史關閉仍可用";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"},
                             {"ContextHistoryLimit", "0"}});
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"歷史關閉仍可用");
        std::filesystem::remove(path);
        harness.set_config("ContextHistoryLimit", "1024");
    });

    // An unusable sample (missing file) leaves the self-managed history alone.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        harness.expect_commit("你");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"你");
    });

    // The accessibility source needs no user opt-in: once a usable sample is
    // available it is adopted automatically.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "自動採用";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->context_cache.window(100) == u"自動採用");
        FCITX_ASSERT(status_contains(accessibility_status(instance), "可取得"));
        std::filesystem::remove(path);
    });
}
