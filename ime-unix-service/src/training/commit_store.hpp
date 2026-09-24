#pragma once

#include "pipe/protocol.hpp"
#include "training/commit_crypto.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace ime::unix_service {

// Whether encrypted recording has been set up and whether it is currently on.
struct CommitProtectionStatus {
    bool configured = false;
    bool enabled = false;
};

// Decrypted view of one commit, used by the review and training paths.
struct CommitRecord {
    std::string id;
    std::string committed_at;
    std::string state;
    std::string context;
    std::string answer;
    std::vector<std::string> readings;
    std::vector<bool> manual;
    bool text_available = false;
};

// Read side shared by the service, the CLI, and the manager.
// Creates the tables and adds the encrypted-recording marker when missing.
// Idempotent; needs a writable connection.
void initialize_commit_database(sqlite3* db);
CommitProtectionStatus read_commit_protection(sqlite3* db);

// Holds the derived key material of one verified password. Locks again when
// destroyed, wiping the key. Not copyable or movable: the stored decryption
// view points into the instance.
class CommitCipher final {
public:
    // Verifies the password against the stored parameters. Throws when no
    // password is configured or the password is wrong.
    void unlock(sqlite3* db, const std::string& password);
    bool unlocked() const { return unlocked_; }
    commit_crypto::Decryption decryption() const;
    std::string open(const std::string& ciphertext, const std::string& identity) const;

private:
    bool unlocked_ = false;
    commit_crypto::PublicParameters parameters_{};
    commit_crypto::PrivateKey private_key_{};
};

// When protection is configured the cipher must be unlocked and every row must
// be encrypted. Without protection the legacy plaintext rows stay readable.
std::vector<CommitRecord> read_commits(sqlite3* db, const std::string& state,
                                       const CommitCipher& cipher, int offset, int limit);
std::vector<CommitRecord> read_commits_by_id(sqlite3* db, const std::vector<std::string>& ids,
                                             const CommitCipher& cipher);

// Per-user store. Recording is best effort for the caller, but a record and
// its readings are always written in one transaction. Commits are only stored
// while encrypted recording is enabled; without a configured password nothing
// is written, so plaintext typing never reaches the database.
class CommitStore final {
public:
    explicit CommitStore(std::filesystem::path path = default_path());
    ~CommitStore();
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    bool record(const protocol::RecordCommitRequest& request);
    static std::filesystem::path default_path();

    CommitProtectionStatus protection_status() const;
    bool recording_enabled() const;
    // Sets up the password, converts existing plaintext rows in one
    // transaction, then vacuums and truncates the WAL.
    void configure_password(const std::string& password);
    void set_recording_enabled(bool enabled);
    // Removes every conversation record and the derived parameters, and
    // vacuums the database. Models and training history stay.
    void reset_conversation_data();

private:
    sqlite3* db_ = nullptr;
};

}  // namespace ime::unix_service
