#pragma once

#include "pipe/protocol.hpp"

#include <filesystem>

struct sqlite3;

namespace ime::unix_service {

// Per-user store. Recording is best effort for the caller, but a record and
// its readings are always written in one transaction.
class CommitStore final {
public:
    explicit CommitStore(std::filesystem::path path = default_path());
    ~CommitStore();
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    bool record(const protocol::RecordCommitRequest& request);
    static std::filesystem::path default_path();

private:
    sqlite3* db_ = nullptr;
};

}  // namespace ime::unix_service
