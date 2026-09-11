#include "pipe/protocol.hpp"
#include "session/session_manager.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <utility>

namespace {

bool protocol_test() {
    using namespace ime::unix_service::protocol;
    OpenSessionResponse opened;
    for (std::size_t i = 0; i < opened.session_id.size(); ++i) {
        opened.session_id[i] = static_cast<std::uint8_t>(i + 1);
        opened.service_epoch[i] = static_cast<std::uint8_t>(0xf0U - i);
    }
    const auto bytes = encode(Message{opened});
    const ByteVector expected{34, 0, 0, 0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                              0xf0, 0xef, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4,
                              0xe3, 0xe2, 0xe1};
    if (bytes != expected) return false;
    auto decoded = decode(bytes);
    return std::get<OpenSessionResponse>(decoded).service_epoch == opened.service_epoch;
}

bool core_adapter_test() {
    ime::unix_service::protocol::PredictRequest request;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    request.padding.push_back({true, {}, U'好'});

    const auto core_padding = ime::unix_service::detail::to_core_padding(request);
    if (core_padding.size() != 2 || core_padding[0].chosen || core_padding[0].bopomofo != u"ㄋㄧˇ" ||
        !core_padding[1].chosen || core_padding[1].chosen_char != U'好') {
        return false;
    }

    std::vector<llavon::ime::core::Prediction> predictions(2);
    predictions[0].candidates = {{U'你', 0.75F}, {U'擬', 0.25F}};
    const auto candidates = ime::unix_service::detail::to_protocol_candidates(request, predictions);
    return candidates == std::vector<std::vector<char32_t>>{{U'你', U'擬'}, {U'好'}};
}

bool core_runtime_test() {
    ime::unix_service::RuntimeConfig config;
    config.model_path = std::filesystem::path(IME_UNIX_SERVICE_TEST_TABLE_DIR) / "bopomofo_char.json";
    config.tables_dir = IME_UNIX_SERVICE_TEST_TABLE_DIR;
    ime::unix_service::CoreRuntime runtime(std::move(config));
    runtime.validate_configuration();
    return !runtime.loaded();
}

class MockEngine final : public ime::unix_service::ISessionEngine {
public:
    std::vector<std::vector<char32_t>> predict(const ime::unix_service::protocol::PredictRequest& request) override {
        std::vector<std::vector<char32_t>> result;
        for (const auto& entry : request.padding) result.push_back(entry.chosen ? std::vector<char32_t>{entry.chosen_char} : std::vector<char32_t>{U'你'});
        return result;
    }
    bool loaded() const noexcept override { return true; }
};

bool session_test() {
    ime::unix_service::SessionLimits limits;
    limits.max_sessions = 2;
    limits.max_idle_sessions = 2;
    limits.idle_timeout = std::chrono::seconds(60);
    limits.max_concurrent_predictions = 2;
    ime::unix_service::SessionManager manager(nullptr, limits, []() { return std::make_unique<MockEngine>(); });

    const auto first = manager.open_session(1000);
    const auto second = manager.open_session(1000);
    if (!std::holds_alternative<ime::unix_service::protocol::OpenSessionResponse>(first) ||
        !std::holds_alternative<ime::unix_service::protocol::OpenSessionResponse>(second)) return false;
    const auto first_id = std::get<ime::unix_service::protocol::OpenSessionResponse>(first).session_id;

    ime::unix_service::protocol::PredictRequest request;
    request.session_id = first_id;
    request.request_id = 1;
    request.buffer_revision = 9;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    const auto prediction = manager.predict(1000, request);
    if (!std::holds_alternative<ime::unix_service::protocol::Prediction>(prediction)) return false;
    const auto& value = std::get<ime::unix_service::protocol::Prediction>(prediction);
    if (value.candidates.size() != 1 || value.candidates.front().empty() || value.candidates.front().front() != U'你') return false;

    const auto duplicate = manager.predict(1000, request);
    if (!std::holds_alternative<ime::unix_service::protocol::Error>(duplicate) ||
        std::get<ime::unix_service::protocol::Error>(duplicate).code !=
            ime::unix_service::protocol::ErrorCode::OutOfOrder) return false;
    const auto unauthorized = manager.status(2000, first_id);
    if (!std::holds_alternative<ime::unix_service::protocol::Error>(unauthorized) ||
        std::get<ime::unix_service::protocol::Error>(unauthorized).code !=
            ime::unix_service::protocol::ErrorCode::Unauthorized) return false;

    const auto closed = manager.close_session(1000, first_id);
    if (!std::holds_alternative<ime::unix_service::protocol::CloseSessionResponse>(closed)) return false;
    return manager.session_count() == 1;
}

}  // namespace

int main() {
    return protocol_test() && core_adapter_test() && core_runtime_test() && session_test() ? EXIT_SUCCESS
                                                                                           : EXIT_FAILURE;
}
