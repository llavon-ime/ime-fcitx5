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

#include <iostream>

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
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "commits.sqlite3";
    const std::string password = "correct horse battery";
    const std::string legacy_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    bool good = false;
    sqlite3* db = nullptr;
    try {
        // A database written before encrypted recording existed. Its readable
        // rows must convert in place when the password is set.
        {
            sqlite3* legacy = nullptr;
            if (sqlite3_open_v2(path.c_str(), &legacy, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
                throw std::runtime_error("could not create legacy database");
            const char* schema =
                "CREATE TABLE commits (id TEXT PRIMARY KEY, context TEXT NOT NULL, answer TEXT NOT NULL, "
                "state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','excluded','trained')), "
                "committed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
                "CREATE TABLE readings (commit_id TEXT NOT NULL REFERENCES commits(id) ON DELETE CASCADE, "
                "position INTEGER NOT NULL, reading TEXT NOT NULL, character INTEGER NOT NULL, "
                "manually_selected INTEGER NOT NULL CHECK(manually_selected IN (0,1)), PRIMARY KEY(commit_id,position));"
                "INSERT INTO commits (id,context,answer,state) VALUES ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','早安','你好','trained');"
                "INSERT INTO readings VALUES ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',0,'ㄋㄧˇ',20320,1),"
                "('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1,'ㄏㄠˇ',22909,0);";
            char* message = nullptr;
            if (sqlite3_exec(legacy, schema, nullptr, nullptr, &message) != SQLITE_OK) {
                const std::string error = message ? message : "legacy schema failed";
                sqlite3_free(message);
                sqlite3_close(legacy);
                throw std::runtime_error(error);
            }
            sqlite3_close(legacy);
        }
        {
            CommitStore store(path);
            if (store.protection_status().configured || store.recording_enabled())
                throw std::runtime_error("fresh store claims protection");
            // Without a password nothing is stored, so typing never reaches
            // the database in readable form.
            if (store.record(request)) throw std::runtime_error("stored a commit without a password");
            store.configure_password(password);
            const auto status = store.protection_status();
            if (!status.configured || !status.enabled) throw std::runtime_error("password was not stored");
            if (!store.record(request)) throw std::runtime_error("could not record commit");
            if (store.record(request)) throw std::runtime_error("duplicate commit");
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
            // Disabling stops storage without touching what is already there.
            store.set_recording_enabled(false);
            auto disabled = request;
            disabled.event_id[0] += 4;
            if (store.record(disabled)) throw std::runtime_error("stored while collection was disabled");
            store.set_recording_enabled(true);
        }
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            throw std::runtime_error("could not inspect commit database");
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db,
                    "SELECT (SELECT COUNT(*) FROM commits), (SELECT COUNT(*) FROM readings), "
                    "(SELECT COUNT(*) FROM commits WHERE schema_version=2), "
                    "(SELECT COUNT(*) FROM commits WHERE answer='你好' OR context='早安'), "
                    "(SELECT COUNT(*) FROM readings WHERE reading='ㄋㄧˇ' OR character != 0)",
                    -1, &stmt, nullptr) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW)
                throw std::runtime_error("could not inspect commit database");
            good = sqlite3_column_int(stmt, 0) == 4 && sqlite3_column_int(stmt, 1) == 7 &&
                   sqlite3_column_int(stmt, 2) == 4 && sqlite3_column_int(stmt, 3) == 0 && sqlite3_column_int(stmt, 4) == 0;
            sqlite3_finalize(stmt);
        }
        {
            const auto find = [](const std::vector<CommitRecord>& records, const std::string& id) -> const CommitRecord* {
                for (const auto& record : records) if (record.id == id) return &record;
                return nullptr;
            };
            CommitCipher locked;
            bool refused = false;
            try { (void)read_commits(db, "pending", locked, 0, 10); }
            catch (const std::runtime_error&) { refused = true; }
            if (!refused) throw std::runtime_error("read sealed records without a password");
            bool wrong = false;
            try { CommitCipher cipher; cipher.unlock(db, "wrong password"); }
            catch (const std::runtime_error&) { wrong = true; }
            if (!wrong) throw std::runtime_error("wrong password was accepted");
            CommitCipher cipher;
            cipher.unlock(db, password);
            std::vector<CommitRecord> pending, migrated;
            try { pending = read_commits(db, "pending", cipher, 0, 10); }
            catch (const std::exception& error) {
                throw std::runtime_error(std::string("pending read failed: ") + error.what());
            }
            std::string recorded_id = "45";
            recorded_id.append(30, '0');
            const auto* recorded = find(pending, recorded_id);
            if (recorded == nullptr || recorded->context != "早安" || recorded->answer != "你好" ||
                recorded->readings.size() != 2 || recorded->readings.front() != "ㄋㄧˇ" ||
                recorded->manual.front() || !recorded->manual.back())
                throw std::runtime_error("decrypted records differ");
            try { migrated = read_commits(db, "trained", cipher, 0, 10); }
            catch (const std::exception& error) {
                throw std::runtime_error(std::string("migrated read failed: ") + error.what());
            }
            if (migrated.size() != 1 || migrated.front().id != legacy_id ||
                migrated.front().context != "早安" || migrated.front().answer != "你好" ||
                migrated.front().readings.size() != 2)
                throw std::runtime_error("legacy rows were not converted");
        }
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
        CommitCipher cipher;
        cipher.unlock(db, password);
        const auto decryption = cipher.decryption();
        NumericDataset dataset;
        try { dataset = write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
        catch (const std::exception& error) { throw std::runtime_error(std::string("dataset failed: ") + error.what()); }
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
        try { (void)write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
        catch (const std::runtime_error&) { mismatched = true; }
        good = good && mismatched;
        // A locked reader cannot turn sealed rows into a dataset.
        bool sealed_refused = false;
        {
            const CommitCipher locked;
            const auto decryption = locked.decryption();
            try { (void)write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
            catch (const std::runtime_error&) { sealed_refused = true; }
        }
        good = good && sealed_refused;
        // Forgetting the password removes the conversation records and the
        // keys; the database stays usable.
        {
            CommitStore store(path);
            store.reset_conversation_data();
            const CommitCipher locked;
            if (store.protection_status().configured ||
                !read_commits(db, "pending", locked, 0, 10).empty()) good = false;
        }
        sqlite3_close(db);
    } catch (const std::exception& error) { std::cerr << "commit test: " << error.what() << '\n'; good = false; }
    catch (...) { std::cerr << "commit test: unknown error\n"; good = false; }
    std::filesystem::remove_all(directory);
    return good;
}

}  // namespace

int main() {
    struct Case { const char* name; bool (*run)(); };
    const Case cases[] = {{"protocol", protocol_test}, {"core-adapter", core_adapter_test},
                          {"core-runtime", core_runtime_test}, {"session", session_test}, {"commit", commit_test}};
    bool good = true;
    for (const auto& item : cases) {
        if (item.run()) continue;
        std::cerr << "failed: " << item.name << '\n';
        good = false;
    }
    return good ? EXIT_SUCCESS : EXIT_FAILURE;
}
