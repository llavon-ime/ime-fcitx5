#include "service/llama_backend.hpp"

#include "config/config.hpp"
#include "service/token_filter.hpp"

#include <cstdlib>
#include <string>
#include <vector>

int run_llama_backend_tests() {
    bool ok = true;

    ime::fcitx5::LlamaBackend backend;
    ok = ok && !backend.ready();

    constexpr int unk = 4;
    std::vector<int> mixed_context = {unk, unk, 42, unk};
    ok = ok && ime::fcitx5::remove_leading_unknown_context_tokens(mixed_context, unk) == 2;
    ok = ok && mixed_context == std::vector<int>({42, unk});

    std::vector<int> known_first_context = {42, unk};
    ok = ok && ime::fcitx5::remove_leading_unknown_context_tokens(known_first_context, unk) == 0;
    ok = ok && known_first_context == std::vector<int>({42, unk});

    std::vector<int> only_unknown_context = {unk, unk};
    ok = ok && ime::fcitx5::remove_leading_unknown_context_tokens(only_unknown_context, unk) == 2;
    ok = ok && only_unknown_context.empty();

    auto cfg = ime::fcitx5::default_config();
    cfg.model_path = "/tmp/llavon-ime-missing-model.gguf";
    bool load_error_reported = false;
    try {
        backend.load(cfg);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        load_error_reported = message.find(cfg.model_path) != std::string::npos &&
                              (message.find("unavailable") != std::string::npos ||
                               message.find("not found") != std::string::npos);
    }
    ok = ok && load_error_reported;

    if (const char* model_path = std::getenv("IME_FCITX5_TEST_MODEL")) {
        auto model_cfg = ime::fcitx5::default_config();
        model_cfg.model_path = model_path;
        model_cfg.context_length = 128;
        model_cfg.thread_count = 2;

        ime::fcitx5::LlamaBackend model_backend;
        model_backend.load(model_cfg);
        ok = ok && model_backend.ready();

        ime::fcitx5::PredictRequest request;
        request.padding.push_back({false, u"ㄋㄧˇ", 0});
        const auto response = model_backend.predict(request);
        ok = ok && response.candidates.size() == 1;
        ok = ok && !response.candidates.front().empty();

        ime::fcitx5::PredictRequest extended_request;
        extended_request.padding.push_back({false, u"ㄋㄧˇ", 0});
        extended_request.padding.push_back({false, u"ㄏㄠˇ", 0});
        const auto extended_response = model_backend.predict(extended_request);
        ok = ok && extended_response.candidates.size() == 2;
        ok = ok && !extended_response.candidates.front().empty();
        ok = ok && !extended_response.candidates.back().empty();
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
