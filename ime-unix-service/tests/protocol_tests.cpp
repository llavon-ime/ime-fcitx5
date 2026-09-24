#include "pipe/protocol.hpp"
#include "session/session_manager.hpp"
#include "training/commit_store.hpp"
#include "training/numeric_dataset.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>
#include <unistd.h>

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

bool commit_test() {
    using namespace ime::unix_service;
    protocol::RecordCommitRequest request;
    request.event_id[0] = 0x45;
    request.context = u"早安";
    request.answer = u"你好";
    request.entries = {{u"ㄋㄧˇ", U'你', false}, {u"ㄏㄠˇ", U'好', true}};
    const auto decoded = std::get<protocol::RecordCommitRequest>(protocol::decode(protocol::encode(request)));
    if (decoded.event_id != request.event_id || decoded.context != request.context ||
        decoded.answer != request.answer || decoded.entries.size() != 2 ||
        !decoded.entries[1].manually_selected) return false;

    const auto directory = std::filesystem::temp_directory_path() /
                           ("llavon-commit-store-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "commits.sqlite3";
    bool good = false;
    try {
        {
            CommitStore store(path);
            if (!store.record(request) || store.record(request)) throw std::runtime_error("duplicate commit");
            auto bad = request; bad.event_id[0]++; bad.answer = u"不符";
            try { (void)store.record(bad); throw std::runtime_error("invalid commit accepted"); }
            catch (const std::invalid_argument&) {}
            auto new_reading = request;
            new_reading.event_id[0] += 2;
            new_reading.answer = u"了";
            new_reading.entries = {{u"ㄌㄜ ", U'了', false}};
            if (!store.record(new_reading)) throw std::runtime_error("could not record new reading");
            auto long_context = request;
            long_context.event_id[0] += 3;
            long_context.context.assign(500, u'你');
            if (!store.record(long_context)) throw std::runtime_error("could not record long context");
        }
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            throw std::runtime_error("could not inspect commit database");
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT (SELECT COUNT(*) FROM commits), (SELECT COUNT(*) FROM readings)",
                               -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            good = sqlite3_column_int(stmt, 0) == 3 && sqlite3_column_int(stmt, 1) == 5;
        sqlite3_finalize(stmt);
        const auto config = directory / "config.json";
        std::ofstream(config) << R"({"vocab_size":18546,"max_position_embeddings":384})";
        nlohmann::json vocab = nlohmann::json::array();
        for (int i = 0; i < 18546; ++i) vocab.push_back("");
        for (const auto name : {"chars", "special_tokens", "bpmf"}) {
            std::ifstream table(std::filesystem::path(IME_UNIX_SERVICE_TEST_TABLE_DIR) / "tokens" /
                                (std::string(name) + ".json"));
            const auto entries = nlohmann::json::parse(table);
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                const auto id = it.value().get<int>();
                if (id < 18546) vocab[id] = it.key();
            }
        }
        std::ofstream(directory / "ime_vocab.json") << nlohmann::json{{"tokens", vocab}}.dump();
        const auto output = directory / "training.jsonl";
        const auto dataset = write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384);
        if (dataset.included_ids.size() != 2 || dataset.skipped != 1 || dataset.pad_token_id != 0) good = false;
        std::ifstream input(output);
        nlohmann::json row;
        input >> row;
        if (row.at("tokens").size() != row.at("candidate_masks").size() ||
            row.at("loss_weights").back() != 1 || row.at("candidate_masks").back().is_null()) good = false;
        for (int copy = 0; copy < 2; ++copy) {
            nlohmann::json repeated;
            input >> repeated;
            if (repeated != row) good = false;
        }
        nlohmann::json long_row;
        input >> long_row;
        if (long_row.at("tokens").size() != 384 || long_row.at("loss_weights").back() != 1) good = false;
        vocab[1427] = "wrong token";  // "你" is token 1427 in this checkpoint.
        std::ofstream(directory / "ime_vocab.json") << nlohmann::json{{"tokens", vocab}}.dump();
        bool mismatched = false;
        try { (void)write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384); }
        catch (const std::runtime_error&) { mismatched = true; }
        good = good && mismatched;
        sqlite3_close(db);
    } catch (...) { good = false; }
    std::filesystem::remove_all(directory);
    return good;
}

}  // namespace

int main() {
    return protocol_test() && core_adapter_test() && core_runtime_test() && session_test() && commit_test() ? EXIT_SUCCESS
                                                                                           : EXIT_FAILURE;
}
