#include "raw_key_harness.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>

using namespace llavon::ime::rawkey;

namespace {

// The accessibility context source is backed by a file in tests, so writing
// the file publishes a sample instead of needing the desktop bus.
std::filesystem::path write_sample(const Harness& harness, std::string_view text) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("llavon-ime-rawkey-sample-" + std::to_string(::getpid()) + ".txt");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return path;
}

HarnessOptions sample_options() {
    HarnessOptions options;
    options.context_sample_path = (std::filesystem::temp_directory_path() /
                                   ("llavon-ime-rawkey-sample-" + std::to_string(::getpid()) + ".txt"))
                                      .string();
    options.enable_accessibility = true;
    return options;
}

}  // namespace

RAWKEY_SUITE("context source", context_source) {
#ifndef __APPLE__
    // Text this IME never committed becomes the prediction context.
    {
        const auto options = sample_options();
        const auto path = write_sample({}, "早安，今天天氣很好。");
        Harness harness(options);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        RAWKEY_ASSERT(harness.session()->context_text == u"早安，今天天氣很好。");
        std::filesystem::remove(path);
    }
#endif

    // Once a client has supplied usable surrounding text, a later empty value
    // is authoritative and clears the previous document prefix.
    {
        Harness harness;
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("客戶端文字", 5, 5);
        harness.type("su3");
        RAWKEY_ASSERT(harness.session()->context_text == u"客戶端文字");
        harness.set_surrounding("", 0, 0);
        harness.type("cl3");
        RAWKEY_ASSERT(harness.session()->context_text.empty());
    }

#ifndef __APPLE__
    // A valid but empty client surrounding text (Electron/Chromium/terminals)
    // must not shadow the accessibility sample.
    {
        const auto options = sample_options();
        const auto path = write_sample({}, "空字串不吃樣本");
        Harness harness(options);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("", 0, 0);
        harness.type("su3");
        RAWKEY_ASSERT(harness.session()->context_text == u"空字串不吃樣本");
        std::filesystem::remove(path);
    }

    // Non-empty client surrounding text stays authoritative and is not
    // replaced by the accessibility sample.
    {
        const auto options = sample_options();
        const auto path = write_sample({}, "樣本不應使用");
        Harness harness(options);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.set_surrounding("客戶端文字", 5, 5);
        harness.type("su3");
        RAWKEY_ASSERT(harness.session()->context_text == u"客戶端文字");
        std::filesystem::remove(path);
    }

    // The accessibility source needs no user opt-in: once a usable sample is
    // available it is adopted automatically.
    {
        const auto options = sample_options();
        const auto path = write_sample({}, "自動採用");
        Harness harness(options);
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.type("su3");
        RAWKEY_ASSERT(harness.session()->context_text == u"自動採用");
        std::filesystem::remove(path);
    }
#endif
}
