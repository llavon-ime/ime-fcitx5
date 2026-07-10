#include "service/service_app.hpp"

#include "config/config.hpp"
#include "protocol/protocol.hpp"

#include <cstdlib>
#include <chrono>

int run_service_app_tests() {
    bool ok = true;

    auto cfg = ime::fcitx5::default_config();
    cfg.model_path.clear();
    ime::fcitx5::ServiceApp app(cfg, "tables/bopomofo_char.json");

    const auto status = app.handle_status();
    ok = ok && status.running;
    ok = ok && !status.model_loaded;
    ok = ok && status.backend == "fallback";

    bool status_does_not_require_table = false;
    try {
        ime::fcitx5::ServiceApp status_only(cfg, "/tmp/llavon-ime-missing-table.json");
        status_does_not_require_table = status_only.handle_status().running;
    } catch (...) {
        status_does_not_require_table = false;
    }
    ok = ok && status_does_not_require_table;

    cfg.idle_timeout_seconds = 1;
    ime::fcitx5::ServiceApp idle_app(cfg, "tables/bopomofo_char.json");
    ok = ok && idle_app.expired_idle_timeout(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    ime::fcitx5::PredictRequest request;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    const auto response = app.handle_predict(request);
    ok = ok && response.candidates.size() == 1;
    ok = ok && !response.candidates.front().empty();

    ok = ok && !app.stop_requested();
    app.handle_stop();
    ok = ok && app.stop_requested();
    ok = ok && !app.handle_status().running;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
