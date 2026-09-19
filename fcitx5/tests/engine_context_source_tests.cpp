#include "engine_harness.hpp"
#include "fcitx5/ime_config.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace llavon::ime::test;

namespace {

const llavon::ime::ImeFcitxConfig* engine_config(fcitx::Instance* instance) {
    auto* addon = instance->addonManager().addon("llavon-ime");
    if (addon == nullptr) return nullptr;
    return static_cast<const llavon::ime::ImeFcitxConfig*>(addon->getConfig());
}

std::string accessibility_status(fcitx::Instance* instance) {
    const auto* config = engine_config(instance);
    return config != nullptr ? *config->accessibilityStatus : std::string();
}

bool status_contains(const std::string& status, std::string_view needle) {
    return status.find(needle) != std::string::npos;
}

}  // namespace

// The engine uses an accessibility caret sample when the client never pushes
// surrounding text. The engine tests run
// with LLAVON_IME_CONTEXT_SAMPLE_FILE, which backs the provider with a file
// instead of the accessibility bus: writing the file publishes a sample.
void engine_test_context_source(fcitx::Instance* instance) {
    const char* sample_path = std::getenv("LLAVON_IME_CONTEXT_SAMPLE_FILE");
    FCITX_ASSERT(sample_path != nullptr && sample_path[0] != '\0');
    const std::filesystem::path path(sample_path);

#ifndef __APPLE__
    // Text this IME never committed becomes the prediction context.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "早安，今天天氣很好。";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->session->context_text == u"早安，今天天氣很好。");
        std::filesystem::remove(path);
    });
#endif

    // Once a client has supplied usable surrounding text, a later empty value
    // is authoritative and clears the previous document prefix.
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("客戶端文字", 5, 5);
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->session->context_text == u"客戶端文字");
        harness.set_surrounding("", 0, 0);
        harness.type("cl3");
        FCITX_ASSERT(harness.engine_state()->session->context_text.empty());
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

#ifndef __APPLE__
    // A valid but empty client surrounding text (Electron/Chromium/terminals)
    // must not shadow the accessibility sample.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "空字串不吃樣本";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("", 0, 0);
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->session->context_text == u"空字串不吃樣本");
        std::filesystem::remove(path);
    });
#endif

    // Non-empty client surrounding text stays authoritative and is not
    // replaced by the accessibility sample.
    instance->eventDispatcher().schedule([instance, path]() {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "樣本不應使用";
        }
        EngineHarness harness(instance);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("客戶端文字", 5, 5);
        harness.type("su3");
        FCITX_ASSERT(harness.engine_state()->session->context_text == u"客戶端文字");
        std::filesystem::remove(path);
    });

#ifndef __APPLE__
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
        FCITX_ASSERT(harness.engine_state()->session->context_text == u"自動採用");
        FCITX_ASSERT(status_contains(accessibility_status(instance), "可取得"));
        std::filesystem::remove(path);
    });
#endif
}
