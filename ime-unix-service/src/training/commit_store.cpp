#include "commit_store.hpp"

#include <sqlite3.h>
#include <utf8/cpp20.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ime::unix_service {
namespace {

void check(int result, sqlite3* db) {
    if (result != SQLITE_OK && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}

void execute(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) { check(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), db); }
    ~Statement() { sqlite3_finalize(stmt_); }
    void text(int index, const std::string& value) {
        check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), db_);
    }
    void integer(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value), db_); }
    void step() { check(sqlite3_step(stmt_), db_); }
    void reset() { check(sqlite3_reset(stmt_), db_); check(sqlite3_clear_bindings(stmt_), db_); }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string event_id(const protocol::SessionId& bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

void validate(const protocol::RecordCommitRequest& request) {
    if (protocol::is_zero(request.event_id) || request.entries.empty() || request.entries.size() > 1024 ||
        request.answer.empty() || request.answer.size() > 1024 || request.context.size() > 4096 ||
        !protocol::valid_utf16(request.answer) || !protocol::valid_utf16(request.context)) {
        throw std::invalid_argument("invalid commit event");
    }
    std::u32string answer;
    for (const auto& entry : request.entries) {
        if (entry.reading.empty() || entry.reading.size() > 64 || !protocol::valid_utf16(entry.reading) ||
            entry.character == 0 || !protocol::valid_scalar(entry.character)) {
            throw std::invalid_argument("invalid commit reading");
        }
        answer.push_back(entry.character);
    }
    if (utf8::utf8to32(utf8::utf16tou8(request.answer)) != answer)
        throw std::invalid_argument("commit answer does not match readings");
}

}  // namespace

std::filesystem::path CommitStore::default_path() {
    if (const char* path = std::getenv("LLAVON_IME_TRAINING_DATABASE_PATH"); path && *path) return path;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return std::filesystem::path(state) / "llavon-ime" / "training" / "commits.sqlite3";
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    return std::filesystem::path(home) / "Library" / "Application Support" / "llavon-ime" / "training" / "commits.sqlite3";
#else
    return std::filesystem::path(home) / ".local" / "state" / "llavon-ime" / "training" / "commits.sqlite3";
#endif
}

CommitStore::CommitStore(std::filesystem::path path) {
    std::filesystem::create_directories(path.parent_path());
    struct stat status {};
    if (::lstat(path.parent_path().c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::getuid())
        throw std::runtime_error("training data directory must belong to the current user");
    if (::chmod(path.parent_path().c_str(), 0700) != 0) throw std::runtime_error("cannot protect training directory");
    if (::lstat(path.c_str(), &status) == 0 && (!S_ISREG(status.st_mode) || status.st_uid != ::getuid() || S_ISLNK(status.st_mode)))
        throw std::runtime_error("unsafe training database path");
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot open training database";
        sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(message);
    }
    try {
        if (::chmod(path.c_str(), 0600) != 0) throw std::runtime_error("cannot protect training database");
        sqlite3_busy_timeout(db_, 3000);
        execute(db_, "PRAGMA foreign_keys=ON");
        execute(db_, "PRAGMA journal_mode=WAL");
        execute(db_, "CREATE TABLE IF NOT EXISTS commits (id TEXT PRIMARY KEY, context TEXT NOT NULL, answer TEXT NOT NULL, "
                     "state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','excluded','trained')), "
                     "committed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))) ");
        execute(db_, "CREATE TABLE IF NOT EXISTS readings (commit_id TEXT NOT NULL REFERENCES commits(id) ON DELETE CASCADE, "
                     "position INTEGER NOT NULL, reading TEXT NOT NULL, character INTEGER NOT NULL, "
                     "manually_selected INTEGER NOT NULL CHECK(manually_selected IN (0,1)), "
                     "PRIMARY KEY(commit_id,position))");
        execute(db_, "CREATE INDEX IF NOT EXISTS commits_state_time ON commits(state,committed_at,id)");
        execute(db_, "CREATE TABLE IF NOT EXISTS lora_runs (id INTEGER PRIMARY KEY, base_revision TEXT NOT NULL, "
                     "adapter_path TEXT NOT NULL, model_path TEXT NOT NULL, record_count INTEGER NOT NULL, "
                     "completed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')))");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}

CommitStore::~CommitStore() { if (db_) sqlite3_close(db_); }

bool CommitStore::record(const protocol::RecordCommitRequest& request) {
    validate(request);
    const auto id = event_id(request.event_id);
    execute(db_, "BEGIN IMMEDIATE");
    try {
        Statement insert(db_, "INSERT OR IGNORE INTO commits(id,context,answer) VALUES (?,?,?)");
        insert.text(1, id); insert.text(2, utf8::utf16to8(request.context));
        insert.text(3, utf8::utf16to8(request.answer)); insert.step();
        const bool inserted = sqlite3_changes(db_) != 0;
        if (inserted) {
            Statement reading(db_, "INSERT INTO readings(commit_id,position,reading,character,manually_selected) VALUES (?,?,?,?,?)");
            for (std::size_t i = 0; i < request.entries.size(); ++i) {
                reading.text(1, id); reading.integer(2, static_cast<std::int64_t>(i));
                reading.text(3, utf8::utf16to8(request.entries[i].reading));
                reading.integer(4, request.entries[i].character);
                reading.integer(5, request.entries[i].manually_selected); reading.step(); reading.reset();
            }
        }
        execute(db_, "COMMIT");
        return inserted;
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

}  // namespace ime::unix_service
