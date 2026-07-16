#include "training/feedback_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace imesvc::training {
namespace {

constexpr int kSchemaVersion = 2;
constexpr std::size_t kMaxEventIdBytes = 256;
constexpr std::size_t kMaxMetadataBytes = 4096;

std::int64_t unix_seconds() noexcept {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string sqlite_error(sqlite3* database, std::string_view operation) {
    std::string error(operation);
    if (database != nullptr) {
        const char* detail = sqlite3_errmsg(database);
        if (detail != nullptr && detail[0] != '\0') {
            error += ": ";
            error += detail;
        }
    }
    return error;
}

StoreOperationResult operation_failure(std::string error) {
    return {false, std::move(error)};
}

StoreOperationResult operation_success() { return {true, {}}; }

template <typename Result>
Result failed_result(std::string error) {
    if constexpr (std::is_same_v<Result, StoreOperationResult>) {
        return operation_failure(std::move(error));
    } else if constexpr (std::is_same_v<Result, RetentionResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else if constexpr (std::is_same_v<Result, SnapshotResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else if constexpr (std::is_same_v<Result, SnapshotLoadResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else if constexpr (std::is_same_v<Result, AdapterLookupResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else if constexpr (std::is_same_v<Result, IncompleteTrainingRunsResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else if constexpr (std::is_same_v<Result, AdapterFeedbackStatsResult>) {
        Result result;
        result.operation = operation_failure(std::move(error));
        return result;
    } else {
        static_assert(std::is_same_v<Result, TrainingAccounting>);
        return {};
    }
}

template <typename Result>
std::future<Result> ready_future(Result result) {
    std::promise<Result> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(result));
    return future;
}

bool is_valid_utf8(std::string_view value, std::size_t* scalar_count = nullptr,
                   std::vector<std::uint32_t>* decoded = nullptr) noexcept {
    std::size_t scalars = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if (byte <= 0x7fU) {
            continuation_count = 0;
            codepoint = byte;
        } else if (byte >= 0xc2U && byte <= 0xdfU) {
            continuation_count = 1;
            codepoint = byte & 0x1fU;
        } else if (byte >= 0xe0U && byte <= 0xefU) {
            continuation_count = 2;
            codepoint = byte & 0x0fU;
        } else if (byte >= 0xf0U && byte <= 0xf4U) {
            continuation_count = 3;
            codepoint = byte & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((continuation_count == 2 && codepoint < 0x800U) ||
            (continuation_count == 3 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
            return false;
        }
        if (decoded != nullptr) decoded->push_back(codepoint);
        ++scalars;
        index += continuation_count + 1;
    }
    if (scalar_count != nullptr) *scalar_count = scalars;
    return true;
}

bool valid_signal(FeedbackSignal signal) noexcept {
    switch (signal) {
        case FeedbackSignal::ExplicitCorrection:
        case FeedbackSignal::ExplicitTop1Selection:
        case FeedbackSignal::AcceptedPrediction:
        case FeedbackSignal::FallbackCommit:
            return true;
    }
    return false;
}

std::uint64_t correction_character_count(const FeedbackEvent& event) {
    std::vector<std::uint32_t> committed;
    std::vector<std::uint32_t> predicted;
    if (!is_valid_utf8(event.committed_characters, nullptr, &committed) ||
        !is_valid_utf8(event.predicted_top1, nullptr, &predicted) || committed.size() != predicted.size() ||
        committed.size() != event.manually_chosen_flags.size()) {
        throw std::runtime_error("feedback correction fields are inconsistent");
    }
    std::uint64_t corrections = 0;
    for (std::size_t index = 0; index < committed.size(); ++index) {
        if (event.manually_chosen_flags[index] != 0 && committed[index] != predicted[index]) ++corrections;
    }
    return corrections;
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite_error(database, "prepare SQLite statement"));
        }
    }

    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

void require_sqlite(int result, sqlite3* database, std::string_view operation) {
    if (result != SQLITE_OK) throw std::runtime_error(sqlite_error(database, operation));
}

void step_done(Statement& statement, sqlite3* database, std::string_view operation) {
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite_error(database, operation));
}

void bind_text(Statement& statement, int index, const std::string& value, sqlite3* database) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("SQLite text value is too large");
    }
    require_sqlite(sqlite3_bind_text(statement.get(), index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
                   database, "bind SQLite text");
}

void bind_blob(Statement& statement, int index, const std::vector<std::uint8_t>& value, sqlite3* database) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("SQLite blob value is too large");
    }
    const void* data = value.empty() ? nullptr : value.data();
    require_sqlite(sqlite3_bind_blob(statement.get(), index, data, static_cast<int>(value.size()), SQLITE_TRANSIENT), database,
                   "bind SQLite blob");
}

void bind_i64(Statement& statement, int index, std::int64_t value, sqlite3* database) {
    require_sqlite(sqlite3_bind_int64(statement.get(), index, value), database, "bind SQLite integer");
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* text = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (text == nullptr || bytes < 0) throw std::runtime_error("invalid SQLite text column");
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

std::vector<std::uint8_t> column_blob(sqlite3_stmt* statement, int column) {
    const int bytes = sqlite3_column_bytes(statement, column);
    if (bytes < 0) throw std::runtime_error("invalid SQLite blob column");
    if (bytes == 0) return {};
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
    if (data == nullptr) throw std::runtime_error("invalid SQLite blob column");
    return {data, data + bytes};
}

std::uint64_t checked_u64(std::int64_t value, std::string_view field) {
    if (value < 0) throw std::runtime_error(std::string("invalid ") + std::string(field));
    return static_cast<std::uint64_t>(value);
}

void execute(sqlite3* database, const char* sql, std::string_view operation) {
    char* message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string error(operation);
        if (message != nullptr) {
            error += ": ";
            error += message;
        }
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}

class Transaction final {
public:
    Transaction(sqlite3* database, const char* begin_statement) : database_(database) { execute(database_, begin_statement, "begin transaction"); }
    ~Transaction() {
        if (!committed_) {
            try {
                execute(database_, "ROLLBACK", "rollback transaction");
            } catch (...) {
            }
        }
    }

    void commit() {
        execute(database_, "COMMIT", "commit transaction");
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_ = false;
};

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_string(std::vector<std::uint8_t>& output, std::string_view value) {
    append_u64(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

void append_bytes(std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& value) {
    append_u64(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned int amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

std::array<std::uint8_t, 32> sha256(std::vector<std::uint8_t> data) {
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) data.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>(bit_length >> shift));

    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t offset = 0; offset < data.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(data[offset + index * 4U]) << 24U) |
                           (static_cast<std::uint32_t>(data[offset + index * 4U + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(data[offset + index * 4U + 2U]) << 8U) |
                           static_cast<std::uint32_t>(data[offset + index * 4U + 3U]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto sigma0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const auto sigma1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
        }
        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temporary1 = h + sigma1 + choice + kSha256Constants[index] + words[index];
            const auto sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        digest[index * 4U] = static_cast<std::uint8_t>(hash[index] >> 24U);
        digest[index * 4U + 1U] = static_cast<std::uint8_t>(hash[index] >> 16U);
        digest[index * 4U + 2U] = static_cast<std::uint8_t>(hash[index] >> 8U);
        digest[index * 4U + 3U] = static_cast<std::uint8_t>(hash[index]);
    }
    return digest;
}

std::string hex_digest(const std::array<std::uint8_t, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        value.push_back(digits[byte >> 4U]);
        value.push_back(digits[byte & 0x0fU]);
    }
    return value;
}

std::string snapshot_digest(const SnapshotMetadata& metadata, const std::vector<DatasetSample>& samples) {
    std::vector<std::uint8_t> data;
    data.reserve(128U + samples.size() * 128U);
    append_string(data, "imesvc-feedback-snapshot-v1");
    append_string(data, metadata.snapshot_id);
    append_u64(data, static_cast<std::uint64_t>(metadata.created_at_unix_seconds));
    append_u64(data, metadata.total_samples);
    append_u64(data, metadata.total_target_characters);
    append_u64(data, metadata.training_target_characters);
    append_u64(data, metadata.validation_target_characters);
    append_u64(data, metadata.validation_samples);
    for (const auto& sample : samples) {
        append_string(data, sample.event_id);
        append_string(data, sample.left_context);
        append_string(data, sample.bopomofo_sequence);
        append_string(data, sample.committed_characters);
        append_string(data, sample.predicted_top1);
        append_bytes(data, sample.manually_chosen_flags);
        data.push_back(static_cast<std::uint8_t>(sample.signal_type));
        append_string(data, sample.base_model_hash);
        append_string(data, sample.adapter_version);
        append_u64(data, static_cast<std::uint64_t>(sample.created_at_unix_seconds));
        append_u64(data, sample.target_characters);
        data.push_back(sample.validation_member ? 1U : 0U);
    }
    return hex_digest(sha256(std::move(data)));
}

std::string random_identifier() {
    std::array<std::uint8_t, 32> bytes{};
    try {
        std::random_device random;
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    } catch (...) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto seed = static_cast<std::uint64_t>(unix_seconds()) ^ ++sequence;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>(seed >> ((index % 8U) * 8U));
        }
    }
    return hex_digest(bytes);
}

void validate_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) throw std::runtime_error("create feedback data directory failed: " + error.message());
#ifndef _WIN32
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0 || !S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
        throw std::runtime_error("feedback data path is not a directory");
    }
    if (::chmod(path.c_str(), S_IRWXU) != 0) throw std::runtime_error("set feedback data directory permissions failed");
#endif
}

void validate_private_database(const std::filesystem::path& path) {
#ifndef _WIN32
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode) || S_ISLNK(information.st_mode)) {
        throw std::runtime_error("feedback database path is not a regular file");
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) throw std::runtime_error("set feedback database permissions failed");
#else
    (void)path;
#endif
}

void validate_database_path_before_open(const std::filesystem::path& path) {
#ifndef _WIN32
    struct stat information {};
    if (::lstat(path.c_str(), &information) == 0) {
        if (!S_ISREG(information.st_mode) || S_ISLNK(information.st_mode)) {
            throw std::runtime_error("feedback database path is not a regular file");
        }
        return;
    }
    if (errno != ENOENT) throw std::runtime_error("inspect feedback database path failed");
#else
    (void)path;
#endif
}

bool valid_filename(const std::string& filename) {
    const std::filesystem::path path(filename);
    return !filename.empty() && !path.is_absolute() && path.parent_path().empty() && filename != "." && filename != "..";
}

DatasetSample sample_from_statement(sqlite3_stmt* statement, int offset) {
    DatasetSample sample;
    sample.event_id = column_text(statement, offset);
    sample.left_context = column_text(statement, offset + 1);
    sample.bopomofo_sequence = column_text(statement, offset + 2);
    sample.committed_characters = column_text(statement, offset + 3);
    sample.predicted_top1 = column_text(statement, offset + 4);
    sample.manually_chosen_flags = column_blob(statement, offset + 5);
    const auto signal = sqlite3_column_int64(statement, offset + 6);
    if (signal < static_cast<std::int64_t>(FeedbackSignal::ExplicitCorrection) ||
        signal > static_cast<std::int64_t>(FeedbackSignal::FallbackCommit)) {
        throw std::runtime_error("invalid feedback signal in database");
    }
    sample.signal_type = static_cast<FeedbackSignal>(signal);
    sample.base_model_hash = column_text(statement, offset + 7);
    sample.adapter_version = column_text(statement, offset + 8);
    sample.created_at_unix_seconds = sqlite3_column_int64(statement, offset + 9);
    const auto targets = checked_u64(sqlite3_column_int64(statement, offset + 10), "target character count");
    if (targets > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("target character count is too large");
    sample.target_characters = static_cast<std::uint32_t>(targets);
    const auto validation = sqlite3_column_int(statement, offset + 11);
    if (validation != 0 && validation != 1) throw std::runtime_error("invalid validation membership in database");
    sample.validation_member = validation != 0;
    return sample;
}

bool sane_sample(const DatasetSample& sample, const FeedbackStoreOptions& options) {
    std::size_t target_characters = 0;
    return !sample.event_id.empty() && sample.event_id.size() <= kMaxEventIdBytes && sample.left_context.size() <= options.max_context_bytes &&
           !sample.bopomofo_sequence.empty() && sample.bopomofo_sequence.size() <= options.max_bopomofo_bytes &&
           !sample.committed_characters.empty() && sample.committed_characters.size() <= options.max_target_bytes &&
           sample.predicted_top1.size() <= options.max_target_bytes && sample.base_model_hash.size() <= kMaxMetadataBytes &&
           sample.adapter_version.size() <= kMaxMetadataBytes && is_valid_utf8(sample.event_id) && is_valid_utf8(sample.left_context) &&
           is_valid_utf8(sample.bopomofo_sequence) && is_valid_utf8(sample.committed_characters, &target_characters) &&
           is_valid_utf8(sample.predicted_top1) && is_valid_utf8(sample.base_model_hash) && is_valid_utf8(sample.adapter_version) &&
           sample.target_characters == target_characters && sample.manually_chosen_flags.size() == target_characters &&
           std::all_of(sample.manually_chosen_flags.begin(), sample.manually_chosen_flags.end(),
                       [](std::uint8_t value) { return value <= 1U; }) &&
           sample.validation_member == FeedbackStore::deterministic_validation_member(sample.event_id) && valid_signal(sample.signal_type);
}

}  // namespace

FeedbackEligibility FeedbackEligibility::approved_sample() noexcept {
    FeedbackEligibility eligibility;
    eligibility.approved = true;
    eligibility.sensitive_context = false;
    eligibility.cancelled = false;
    eligibility.complete_bopomofo = true;
    eligibility.one_to_one_alignment = true;
    eligibility.targets_are_legal = true;
    eligibility.excessive_unknown_tokens = false;
    eligibility.pathological_input = false;
    return eligibility;
}

class FeedbackStore::Impl final {
public:
    explicit Impl(FeedbackStoreOptions options) : options_(std::move(options)) {
        data_directory_ = options_.data_directory.empty() ? FeedbackStore::default_data_directory() : options_.data_directory;
        std::error_code error;
        data_directory_ = std::filesystem::absolute(data_directory_, error);
        if (error) initialization_error_ = "resolve feedback data directory failed: " + error.message();
        if (!valid_filename(options_.database_filename)) initialization_error_ = "database filename must name a file in the feedback data directory";
        database_path_ = data_directory_ / options_.database_filename;

        std::promise<StoreOperationResult> initialized;
        auto future = initialized.get_future();
        writer_ = std::thread([this, initialized = std::move(initialized)]() mutable {
            StoreOperationResult result;
            try {
                result = initialize_database();
            } catch (const std::exception& error) {
                result = operation_failure(error.what());
            } catch (...) {
                result = operation_failure("unknown feedback database initialization failure");
            }
            if (!result.succeeded) initialization_error_ = result.error;
            available_.store(result.succeeded, std::memory_order_release);
            initialized.set_value(result);
            writer_loop();
        });
        const auto result = future.get();
        if (!result.succeeded) learning_enabled_.store(false, std::memory_order_release);
    }

    ~Impl() {
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
        }
        queue_ready_.notify_all();
        queue_space_.notify_all();
        if (writer_.joinable()) writer_.join();
    }

    template <typename Result, typename Function>
    std::future<Result> submit(Function&& function, bool wait_for_space = false, bool require_available = true) {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::unique_lock lock(queue_mutex_);
        if (options_.queue_capacity == 0) {
            promise->set_value(failed_result<Result>("feedback writer queue capacity is zero"));
            return future;
        }
        if (wait_for_space) {
            queue_space_.wait(lock, [this] { return stopping_ || queue_.size() < options_.queue_capacity; });
        }
        if (stopping_) {
            promise->set_value(failed_result<Result>("feedback store is shutting down"));
            return future;
        }
        if (require_available && !available_.load(std::memory_order_acquire)) {
            promise->set_value(failed_result<Result>(initialization_error_.empty() ? "feedback database is unavailable" : initialization_error_));
            return future;
        }
        if (queue_.size() >= options_.queue_capacity) {
            promise->set_value(failed_result<Result>("feedback writer queue is full"));
            return future;
        }
        queue_.emplace_back([promise, function = std::forward<Function>(function)](sqlite3* database) mutable {
            try {
                promise->set_value(function(database));
            } catch (const std::exception& error) {
                promise->set_value(failed_result<Result>(error.what()));
            } catch (...) {
                promise->set_value(failed_result<Result>("unknown feedback writer failure"));
            }
        });
        lock.unlock();
        queue_ready_.notify_one();
        return future;
    }

    FeedbackEnqueueResult enqueue(FeedbackEvent event) {
        if (!available_.load(std::memory_order_acquire)) return {FeedbackEnqueueStatus::Invalid, "feedback database is unavailable"};
        if (!learning_enabled_.load(std::memory_order_acquire)) return {FeedbackEnqueueStatus::LearningDisabled, "personal learning is disabled"};
        std::string reason;
        if (!validate_event(event, reason)) return {FeedbackEnqueueStatus::Invalid, std::move(reason)};

        std::lock_guard lock(queue_mutex_);
        if (stopping_) return {FeedbackEnqueueStatus::ShuttingDown, "feedback store is shutting down"};
        if (!learning_enabled_.load(std::memory_order_acquire)) {
            return {FeedbackEnqueueStatus::LearningDisabled, "personal learning is disabled"};
        }
        if (queue_.size() >= options_.queue_capacity) return {FeedbackEnqueueStatus::QueueFull, "feedback writer queue is full"};
        queue_.emplace_back([this, event = std::move(event)](sqlite3* database) mutable {
            try {
                if (insert_event(database, event)) {
                    ++successful_event_writes_;
                    if (options_.retention_check_interval != 0 &&
                        successful_event_writes_ % options_.retention_check_interval == 0) {
                        (void)apply_retention(database);
                    }
                }
            } catch (...) {
                available_.store(false, std::memory_order_release);
                learning_enabled_.store(false, std::memory_order_release);
            }
        });
        queue_ready_.notify_one();
        return {FeedbackEnqueueStatus::Queued, {}};
    }

    [[nodiscard]] bool available() const noexcept { return available_.load(std::memory_order_acquire); }
    [[nodiscard]] bool learning_enabled() const noexcept { return learning_enabled_.load(std::memory_order_acquire); }
    void disable_learning_in_memory() noexcept { learning_enabled_.store(false, std::memory_order_release); }
    [[nodiscard]] const std::filesystem::path& data_directory() const noexcept { return data_directory_; }
    [[nodiscard]] const std::filesystem::path& database_path() const noexcept { return database_path_; }

    StoreOperationResult set_learning_enabled(sqlite3* database, bool enabled) {
        try {
            Statement statement(database, "UPDATE learning_state SET learning_enabled = ? WHERE id = 1");
            require_sqlite(sqlite3_bind_int(statement.get(), 1, enabled ? 1 : 0), database, "bind learning state");
            step_done(statement, database, "update learning state");
            learning_enabled_.store(enabled, std::memory_order_release);
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

    StoreOperationResult delete_all_personal_data(sqlite3* database) {
        learning_enabled_.store(false, std::memory_order_release);
        try {
            if (database == nullptr) throw std::runtime_error("feedback database is unavailable");
            Transaction transaction(database, "BEGIN IMMEDIATE");
            execute(database, "DELETE FROM dataset_snapshots", "delete dataset snapshots");
            execute(database, "DELETE FROM samples", "delete feedback samples");
            execute(database, "DELETE FROM training_runs", "delete training runs");
            execute(database, "DELETE FROM adapters", "delete adapter records");
            execute(database, "UPDATE learning_state SET learning_enabled = 0 WHERE id = 1", "disable personal learning");
            transaction.commit();
            execute(database, "VACUUM", "vacuum deleted personal data");
            int remaining_log_frames = 0;
            int checkpointed_frames = 0;
            require_sqlite(sqlite3_wal_checkpoint_v2(database, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                                     &remaining_log_frames, &checkpointed_frames),
                           database, "truncate feedback WAL");
            if (remaining_log_frames != 0) throw std::runtime_error("feedback WAL still contains frames after deletion");
            learning_enabled_.store(false, std::memory_order_release);
            return operation_success();
        } catch (const std::exception& error) {
            available_.store(false, std::memory_order_release);
            if (database_ != nullptr) {
                (void)sqlite3_close(database_);
                database_ = nullptr;
            }
            for (const auto& path : {database_path_, std::filesystem::path(database_path_.string() + "-wal"),
                                     std::filesystem::path(database_path_.string() + "-shm")}) {
                std::error_code status_error;
                const auto status = std::filesystem::symlink_status(path, status_error);
                if (status_error) {
                    if (status_error == std::errc::no_such_file_or_directory) continue;
                    return operation_failure("inspect personal-data database artifact failed: " + status_error.message());
                }
                if (std::filesystem::is_symlink(status)) {
                    return operation_failure("refusing symlink personal-data database artifact");
                }
                if (!std::filesystem::is_regular_file(status)) {
                    return operation_failure("refusing non-file personal-data database artifact");
                }
                std::error_code remove_error;
                if (!std::filesystem::remove(path, remove_error) || remove_error) {
                    return operation_failure("remove personal-data database artifact after SQLite failure failed: " +
                                             (remove_error ? remove_error.message() : std::string("file remains")));
                }
            }
            (void)error;
            return operation_success();
        }
    }

    RetentionResult apply_retention(sqlite3* database) {
        RetentionResult result;
        try {
            result = apply_retention_impl(database);
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    SnapshotResult create_snapshot(sqlite3* database, SnapshotOptions options) {
        SnapshotResult result;
        try {
            Transaction transaction(database, "BEGIN IMMEDIATE");
            SnapshotMetadata metadata;
            metadata.snapshot_id = random_identifier();
            metadata.created_at_unix_seconds = unix_seconds();
            std::vector<DatasetSample> samples;
            const char* source_sql = options_.base_model_hash.empty()
                                         ? "SELECT event_id, left_context, bopomofo_sequence, committed_characters, predicted_top1, "
                                           "manually_chosen_flags, signal_type, base_model_hash, adapter_version, created_at, "
                                           "target_characters, validation_member FROM samples ORDER BY created_at ASC, event_id ASC"
                                         : "SELECT event_id, left_context, bopomofo_sequence, committed_characters, predicted_top1, "
                                           "manually_chosen_flags, signal_type, base_model_hash, adapter_version, created_at, "
                                           "target_characters, validation_member FROM samples WHERE base_model_hash = ? "
                                           "ORDER BY created_at ASC, event_id ASC";
            Statement source(database, source_sql);
            if (!options_.base_model_hash.empty()) bind_text(source, 1, options_.base_model_hash, database);
            while (sqlite3_step(source.get()) == SQLITE_ROW) {
                DatasetSample sample = sample_from_statement(source.get(), 0);
                if (!sane_sample(sample, options_)) throw std::runtime_error("invalid feedback sample in database");
                if (sample.target_characters > options.max_target_characters ||
                    metadata.total_target_characters > options.max_target_characters - sample.target_characters) {
                    continue;
                }
                if (metadata.total_target_characters > std::numeric_limits<std::uint64_t>::max() - sample.target_characters) {
                    throw std::runtime_error("snapshot target character count overflow");
                }
                samples.push_back(std::move(sample));
                metadata.total_target_characters += samples.back().target_characters;
                if (samples.back().validation_member) {
                    ++metadata.validation_samples;
                    metadata.validation_target_characters += samples.back().target_characters;
                } else {
                    metadata.training_target_characters += samples.back().target_characters;
                }
            }
            if (sqlite3_errcode(database) != SQLITE_DONE) throw std::runtime_error(sqlite_error(database, "read feedback samples"));
            metadata.total_samples = samples.size();
            metadata.sha256 = snapshot_digest(metadata, samples);

            Statement insert_snapshot(database,
                                      "INSERT INTO dataset_snapshots (snapshot_id, sha256, created_at, total_samples, "
                                      "total_target_characters, training_target_characters, validation_target_characters, "
                                      "validation_samples) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
            bind_text(insert_snapshot, 1, metadata.snapshot_id, database);
            bind_text(insert_snapshot, 2, metadata.sha256, database);
            bind_i64(insert_snapshot, 3, metadata.created_at_unix_seconds, database);
            bind_i64(insert_snapshot, 4, static_cast<std::int64_t>(metadata.total_samples), database);
            bind_i64(insert_snapshot, 5, static_cast<std::int64_t>(metadata.total_target_characters), database);
            bind_i64(insert_snapshot, 6, static_cast<std::int64_t>(metadata.training_target_characters), database);
            bind_i64(insert_snapshot, 7, static_cast<std::int64_t>(metadata.validation_target_characters), database);
            bind_i64(insert_snapshot, 8, static_cast<std::int64_t>(metadata.validation_samples), database);
            step_done(insert_snapshot, database, "insert dataset snapshot");

            Statement insert_sample(database,
                                    "INSERT INTO dataset_snapshot_samples (snapshot_id, ordinal, event_id, left_context, "
                                    "bopomofo_sequence, committed_characters, predicted_top1, manually_chosen_flags, "
                                    "signal_type, base_model_hash, adapter_version, created_at, target_characters, "
                                    "validation_member) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            for (std::size_t ordinal = 0; ordinal < samples.size(); ++ordinal) {
                const auto& sample = samples[ordinal];
                require_sqlite(sqlite3_reset(insert_sample.get()), database, "reset snapshot sample statement");
                require_sqlite(sqlite3_clear_bindings(insert_sample.get()), database, "clear snapshot sample bindings");
                bind_text(insert_sample, 1, metadata.snapshot_id, database);
                bind_i64(insert_sample, 2, static_cast<std::int64_t>(ordinal), database);
                bind_text(insert_sample, 3, sample.event_id, database);
                bind_text(insert_sample, 4, sample.left_context, database);
                bind_text(insert_sample, 5, sample.bopomofo_sequence, database);
                bind_text(insert_sample, 6, sample.committed_characters, database);
                bind_text(insert_sample, 7, sample.predicted_top1, database);
                bind_blob(insert_sample, 8, sample.manually_chosen_flags, database);
                bind_i64(insert_sample, 9, static_cast<std::int64_t>(sample.signal_type), database);
                bind_text(insert_sample, 10, sample.base_model_hash, database);
                bind_text(insert_sample, 11, sample.adapter_version, database);
                bind_i64(insert_sample, 12, sample.created_at_unix_seconds, database);
                bind_i64(insert_sample, 13, sample.target_characters, database);
                require_sqlite(sqlite3_bind_int(insert_sample.get(), 14, sample.validation_member ? 1 : 0), database,
                               "bind snapshot validation membership");
                step_done(insert_sample, database, "insert immutable snapshot sample");
            }
            transaction.commit();
            result.operation = operation_success();
            result.snapshot = std::move(metadata);
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    SnapshotLoadResult load_snapshot(sqlite3* database, const std::string& snapshot_id) {
        SnapshotLoadResult result;
        try {
            if (snapshot_id.empty() || snapshot_id.size() > kMaxEventIdBytes) throw std::runtime_error("invalid snapshot id");
            Statement metadata_statement(database,
                                         "SELECT snapshot_id, sha256, created_at, total_samples, total_target_characters, "
                                         "training_target_characters, validation_target_characters, validation_samples "
                                         "FROM dataset_snapshots WHERE snapshot_id = ?");
            bind_text(metadata_statement, 1, snapshot_id, database);
            if (sqlite3_step(metadata_statement.get()) != SQLITE_ROW) throw std::runtime_error("dataset snapshot does not exist");
            auto& metadata = result.snapshot;
            metadata.snapshot_id = column_text(metadata_statement.get(), 0);
            metadata.sha256 = column_text(metadata_statement.get(), 1);
            metadata.created_at_unix_seconds = sqlite3_column_int64(metadata_statement.get(), 2);
            metadata.total_samples = checked_u64(sqlite3_column_int64(metadata_statement.get(), 3), "snapshot sample count");
            metadata.total_target_characters = checked_u64(sqlite3_column_int64(metadata_statement.get(), 4), "snapshot target count");
            metadata.training_target_characters = checked_u64(sqlite3_column_int64(metadata_statement.get(), 5), "snapshot training count");
            metadata.validation_target_characters = checked_u64(sqlite3_column_int64(metadata_statement.get(), 6), "snapshot validation count");
            metadata.validation_samples = checked_u64(sqlite3_column_int64(metadata_statement.get(), 7), "snapshot validation samples");

            Statement samples_statement(database,
                                        "SELECT event_id, left_context, bopomofo_sequence, committed_characters, predicted_top1, "
                                        "manually_chosen_flags, signal_type, base_model_hash, adapter_version, created_at, "
                                        "target_characters, validation_member FROM dataset_snapshot_samples "
                                        "WHERE snapshot_id = ? ORDER BY ordinal ASC");
            bind_text(samples_statement, 1, snapshot_id, database);
            while (sqlite3_step(samples_statement.get()) == SQLITE_ROW) {
                DatasetSample sample = sample_from_statement(samples_statement.get(), 0);
                if (!sane_sample(sample, options_)) throw std::runtime_error("dataset snapshot integrity check failed");
                result.samples.push_back(std::move(sample));
            }
            if (sqlite3_errcode(database) != SQLITE_DONE) throw std::runtime_error(sqlite_error(database, "read immutable snapshot"));

            SnapshotMetadata calculated = metadata;
            calculated.total_samples = result.samples.size();
            calculated.total_target_characters = 0;
            calculated.training_target_characters = 0;
            calculated.validation_target_characters = 0;
            calculated.validation_samples = 0;
            for (const auto& sample : result.samples) {
                calculated.total_target_characters += sample.target_characters;
                if (sample.validation_member) {
                    ++calculated.validation_samples;
                    calculated.validation_target_characters += sample.target_characters;
                } else {
                    calculated.training_target_characters += sample.target_characters;
                }
            }
            if (calculated.total_samples != metadata.total_samples ||
                calculated.total_target_characters != metadata.total_target_characters ||
                calculated.training_target_characters != metadata.training_target_characters ||
                calculated.validation_target_characters != metadata.validation_target_characters ||
                calculated.validation_samples != metadata.validation_samples || snapshot_digest(metadata, result.samples) != metadata.sha256) {
                throw std::runtime_error("dataset snapshot integrity check failed");
            }
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
            result.samples.clear();
        }
        return result;
    }

    TrainingAccounting accounting(sqlite3* database) {
        TrainingAccounting result;
        try {
            result.learning_enabled = learning_enabled_.load(std::memory_order_acquire);
            {
                const char* sql = options_.base_model_hash.empty()
                                      ? "SELECT COUNT(*), COALESCE(SUM(target_characters), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 0 THEN target_characters ELSE 0 END), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 1 THEN target_characters ELSE 0 END), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 1 THEN 1 ELSE 0 END), 0) FROM samples"
                                      : "SELECT COUNT(*), COALESCE(SUM(target_characters), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 0 THEN target_characters ELSE 0 END), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 1 THEN target_characters ELSE 0 END), 0), "
                                        "COALESCE(SUM(CASE WHEN validation_member = 1 THEN 1 ELSE 0 END), 0) FROM samples WHERE base_model_hash = ?";
                Statement statement(database, sql);
                if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
                if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "read training accounting"));
                result.eligible_samples = checked_u64(sqlite3_column_int64(statement.get(), 0), "eligible sample count");
                result.eligible_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 1), "eligible target count");
                result.training_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 2), "training target count");
                result.validation_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 3), "validation target count");
                result.validation_samples = checked_u64(sqlite3_column_int64(statement.get(), 4), "validation sample count");
            }
            {
                const char* sql = options_.base_model_hash.empty()
                                      ? "SELECT EXISTS(SELECT 1 FROM adapters WHERE active = 1), "
                                        "EXISTS(SELECT 1 FROM training_runs WHERE kind = 1 AND succeeded = 1)"
                                      : "SELECT EXISTS(SELECT 1 FROM adapters WHERE active = 1 AND base_model_hash = ?), "
                                        "EXISTS(SELECT 1 FROM training_runs r WHERE r.kind = 1 AND r.succeeded = 1 AND "
                                        "EXISTS(SELECT 1 FROM dataset_snapshot_samples s WHERE s.snapshot_id = r.snapshot_id AND s.base_model_hash = ?))";
                Statement statement(database, sql);
                if (!options_.base_model_hash.empty()) {
                    bind_text(statement, 1, options_.base_model_hash, database);
                    bind_text(statement, 2, options_.base_model_hash, database);
                }
                if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "read training state"));
                result.has_active_adapter = sqlite3_column_int(statement.get(), 0) != 0;
                result.shadow_completed = sqlite3_column_int(statement.get(), 1) != 0;
            }
            {
                const char* sql = options_.base_model_hash.empty()
                                      ? "SELECT COALESCE(MAX(started_at), 0), COALESCE(MAX(completed_at), 0) FROM training_runs"
                                      : "SELECT COALESCE(MAX(r.started_at), 0), COALESCE(MAX(r.completed_at), 0) FROM training_runs r "
                                        "WHERE EXISTS(SELECT 1 FROM dataset_snapshot_samples s WHERE s.snapshot_id = r.snapshot_id AND s.base_model_hash = ?)";
                Statement statement(database, sql);
                if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
                if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "read training times"));
                result.last_training_started_at_unix_seconds = sqlite3_column_int64(statement.get(), 0);
                result.last_training_completed_at_unix_seconds = sqlite3_column_int64(statement.get(), 1);
            }
            {
                const char* sql = options_.base_model_hash.empty()
                                      ? "SELECT succeeded FROM training_runs WHERE completed_at IS NOT NULL "
                                        "ORDER BY completed_at DESC, started_at DESC LIMIT 1"
                                      : "SELECT r.succeeded FROM training_runs r WHERE r.completed_at IS NOT NULL AND "
                                        "EXISTS(SELECT 1 FROM dataset_snapshot_samples s WHERE s.snapshot_id = r.snapshot_id "
                                        "AND s.base_model_hash = ?) ORDER BY r.completed_at DESC, r.started_at DESC LIMIT 1";
                Statement statement(database, sql);
                if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
                if (sqlite3_step(statement.get()) == SQLITE_ROW) {
                    result.last_training_failed = sqlite3_column_int(statement.get(), 0) == 0;
                }
            }
            {
                const char* sql = options_.base_model_hash.empty()
                                      ? "SELECT eligible_target_characters FROM training_runs WHERE succeeded = 1 "
                                        "ORDER BY completed_at DESC, started_at DESC LIMIT 1"
                                      : "SELECT r.eligible_target_characters FROM training_runs r WHERE r.succeeded = 1 AND "
                                        "EXISTS(SELECT 1 FROM dataset_snapshot_samples s WHERE s.snapshot_id = r.snapshot_id AND s.base_model_hash = ?) "
                                        "ORDER BY r.completed_at DESC, r.started_at DESC LIMIT 1";
                Statement statement(database, sql);
                if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
                if (sqlite3_step(statement.get()) == SQLITE_ROW) {
                    result.eligible_target_characters_at_last_success =
                        checked_u64(sqlite3_column_int64(statement.get(), 0), "last successful training target count");
                }
            }
        } catch (...) {
            return {};
        }
        return result;
    }

    StoreOperationResult record_training_started(sqlite3* database, TrainingRunStart run) {
        try {
            if (run.run_id.empty() || run.run_id.size() > kMaxEventIdBytes || run.snapshot_id.empty() ||
                run.snapshot_id.size() > kMaxEventIdBytes || run.started_at_unix_seconds < 0 ||
                (run.kind != TrainingRunKind::ShadowSmoke && run.kind != TrainingRunKind::FirstAdapter && run.kind != TrainingRunKind::Incremental)) {
                return operation_failure("invalid training run");
            }
            if (run.started_at_unix_seconds == 0) run.started_at_unix_seconds = unix_seconds();
            Statement snapshot(database, "SELECT 1 FROM dataset_snapshots WHERE snapshot_id = ?");
            bind_text(snapshot, 1, run.snapshot_id, database);
            if (sqlite3_step(snapshot.get()) != SQLITE_ROW) return operation_failure("training dataset snapshot does not exist");
            Statement statement(database, "INSERT INTO training_runs (run_id, snapshot_id, kind, started_at, completed_at, succeeded, "
                                          "eligible_target_characters, eligible_samples) VALUES (?, ?, ?, ?, NULL, NULL, ?, ?) "
                                          "ON CONFLICT(run_id) DO NOTHING");
            bind_text(statement, 1, run.run_id, database);
            bind_text(statement, 2, run.snapshot_id, database);
            bind_i64(statement, 3, static_cast<std::int64_t>(run.kind), database);
            bind_i64(statement, 4, run.started_at_unix_seconds, database);
            bind_i64(statement, 5, static_cast<std::int64_t>(run.eligible_target_characters), database);
            bind_i64(statement, 6, static_cast<std::int64_t>(run.eligible_samples), database);
            step_done(statement, database, "record training start");
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

    IncompleteTrainingRunsResult incomplete_training_runs(sqlite3* database) {
        IncompleteTrainingRunsResult result;
        try {
            const char* sql = options_.base_model_hash.empty()
                                  ? "SELECT run_id, snapshot_id, kind, started_at, eligible_target_characters, eligible_samples "
                                    "FROM training_runs WHERE completed_at IS NULL ORDER BY started_at DESC, run_id DESC"
                                  : "SELECT r.run_id, r.snapshot_id, r.kind, r.started_at, r.eligible_target_characters, r.eligible_samples "
                                    "FROM training_runs r WHERE r.completed_at IS NULL AND EXISTS("
                                    "SELECT 1 FROM dataset_snapshot_samples s WHERE s.snapshot_id = r.snapshot_id AND s.base_model_hash = ?) "
                                    "ORDER BY r.started_at DESC, r.run_id DESC";
            Statement statement(database, sql);
            if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
            while (sqlite3_step(statement.get()) == SQLITE_ROW) {
                TrainingRunStart run;
                run.run_id = column_text(statement.get(), 0);
                run.snapshot_id = column_text(statement.get(), 1);
                const auto kind = sqlite3_column_int64(statement.get(), 2);
                if (kind < static_cast<std::int64_t>(TrainingRunKind::ShadowSmoke) ||
                    kind > static_cast<std::int64_t>(TrainingRunKind::Incremental)) {
                    throw std::runtime_error("invalid incomplete training run kind");
                }
                run.kind = static_cast<TrainingRunKind>(kind);
                run.started_at_unix_seconds = sqlite3_column_int64(statement.get(), 3);
                run.eligible_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 4), "training target count");
                run.eligible_samples = checked_u64(sqlite3_column_int64(statement.get(), 5), "training sample count");
                result.runs.push_back(std::move(run));
            }
            if (sqlite3_errcode(database) != SQLITE_DONE) throw std::runtime_error(sqlite_error(database, "read incomplete training runs"));
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
            result.runs.clear();
        }
        return result;
    }

    StoreOperationResult record_training_finished(sqlite3* database, const std::string& run_id, bool succeeded,
                                                   std::int64_t finished_at) {
        try {
            if (run_id.empty() || run_id.size() > kMaxEventIdBytes || finished_at < 0) return operation_failure("invalid training completion");
            if (finished_at == 0) finished_at = unix_seconds();
            Statement existing(database, "SELECT completed_at, succeeded FROM training_runs WHERE run_id = ?");
            bind_text(existing, 1, run_id, database);
            if (sqlite3_step(existing.get()) != SQLITE_ROW) return operation_failure("training run does not exist");
            if (sqlite3_column_type(existing.get(), 0) != SQLITE_NULL) {
                return sqlite3_column_int(existing.get(), 1) == (succeeded ? 1 : 0) ? operation_success()
                                                                                       : operation_failure("training run already has a different result");
            }
            Statement statement(database, "UPDATE training_runs SET completed_at = ?, succeeded = ? WHERE run_id = ? AND completed_at IS NULL");
            bind_i64(statement, 1, finished_at, database);
            require_sqlite(sqlite3_bind_int(statement.get(), 2, succeeded ? 1 : 0), database, "bind training result");
            bind_text(statement, 3, run_id, database);
            step_done(statement, database, "record training completion");
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

    StoreOperationResult record_adapter(sqlite3* database, const AdapterRecord& adapter,
                                        const std::string* completed_run_id = nullptr) {
        try {
            if (adapter.version.empty() || !valid_filename(adapter.version) || adapter.version.size() > kMaxEventIdBytes || adapter.dataset_snapshot_id.empty() ||
                adapter.dataset_snapshot_id.size() > kMaxEventIdBytes || adapter.base_model_hash.size() > kMaxMetadataBytes ||
                adapter.sha256.size() > kMaxMetadataBytes || !is_valid_utf8(adapter.version) || !is_valid_utf8(adapter.base_model_hash) ||
                !is_valid_utf8(adapter.dataset_snapshot_id) || !is_valid_utf8(adapter.sha256) || adapter.created_at_unix_seconds < 0) {
                return operation_failure("invalid adapter record");
            }
            if (!options_.base_model_hash.empty() && adapter.base_model_hash != options_.base_model_hash) {
                return operation_failure("adapter base model hash does not match the active service model");
            }
            AdapterRecord stored = adapter;
            if (stored.created_at_unix_seconds == 0) stored.created_at_unix_seconds = unix_seconds();
            Transaction transaction(database, "BEGIN IMMEDIATE");
            Statement snapshot(database, "SELECT 1 FROM dataset_snapshots WHERE snapshot_id = ?");
            bind_text(snapshot, 1, stored.dataset_snapshot_id, database);
            if (sqlite3_step(snapshot.get()) != SQLITE_ROW) return operation_failure("adapter dataset snapshot does not exist");
            if (stored.active) execute(database, "UPDATE adapters SET active = 0 WHERE active = 1", "deactivate adapters");
            Statement statement(database, "INSERT INTO adapters (version, base_model_hash, dataset_snapshot_id, sha256, created_at, active) "
                                          "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(version) DO UPDATE SET "
                                          "base_model_hash = excluded.base_model_hash, dataset_snapshot_id = excluded.dataset_snapshot_id, "
                                          "sha256 = excluded.sha256, created_at = excluded.created_at, active = excluded.active");
            bind_text(statement, 1, stored.version, database);
            bind_text(statement, 2, stored.base_model_hash, database);
            bind_text(statement, 3, stored.dataset_snapshot_id, database);
            bind_text(statement, 4, stored.sha256, database);
            bind_i64(statement, 5, stored.created_at_unix_seconds, database);
            require_sqlite(sqlite3_bind_int(statement.get(), 6, stored.active ? 1 : 0), database, "bind adapter active state");
            step_done(statement, database, "record adapter");
            if (completed_run_id != nullptr) {
                if (completed_run_id->empty() || completed_run_id->size() > kMaxEventIdBytes) {
                    throw std::runtime_error("invalid completed training run id");
                }
                Statement run(database, "UPDATE training_runs SET completed_at = ?, succeeded = 1 "
                                        "WHERE run_id = ? AND snapshot_id = ? AND completed_at IS NULL");
                bind_i64(run, 1, unix_seconds(), database);
                bind_text(run, 2, *completed_run_id, database);
                bind_text(run, 3, stored.dataset_snapshot_id, database);
                step_done(run, database, "complete training run with adapter");
                if (sqlite3_changes(database) != 1) throw std::runtime_error("training run cannot be completed with this adapter");
            }
            transaction.commit();
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

    AdapterLookupResult active_adapter(sqlite3* database) {
        AdapterLookupResult result;
        try {
            const char* sql = options_.base_model_hash.empty()
                                  ? "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active "
                                    "FROM adapters WHERE active = 1 LIMIT 1"
                                  : "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active "
                                    "FROM adapters WHERE active = 1 AND base_model_hash = ? LIMIT 1";
            Statement statement(database, sql);
            if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
            const auto step = sqlite3_step(statement.get());
            if (step == SQLITE_ROW) {
                AdapterRecord adapter;
                adapter.version = column_text(statement.get(), 0);
                adapter.base_model_hash = column_text(statement.get(), 1);
                adapter.dataset_snapshot_id = column_text(statement.get(), 2);
                adapter.sha256 = column_text(statement.get(), 3);
                adapter.created_at_unix_seconds = sqlite3_column_int64(statement.get(), 4);
                adapter.active = sqlite3_column_int(statement.get(), 5) != 0;
                if (!adapter.active) throw std::runtime_error("invalid inactive active-adapter record");
                result.adapter = std::move(adapter);
            } else if (step != SQLITE_DONE) {
                throw std::runtime_error(sqlite_error(database, "read active adapter"));
            }
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    AdapterLookupResult previous_adapter(sqlite3* database, const std::string& version) {
        AdapterLookupResult result;
        try {
            if (version.empty() || version.size() > kMaxEventIdBytes) throw std::runtime_error("invalid adapter version");
            const char* sql = options_.base_model_hash.empty()
                                  ? "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active FROM adapters "
                                    "WHERE rowid < (SELECT rowid FROM adapters WHERE version = ?) ORDER BY rowid DESC LIMIT 1"
                                  : "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active FROM adapters "
                                    "WHERE base_model_hash = ? AND rowid < (SELECT rowid FROM adapters WHERE version = ?) "
                                    "ORDER BY rowid DESC LIMIT 1";
            Statement statement(database, sql);
            int parameter = 1;
            if (!options_.base_model_hash.empty()) bind_text(statement, parameter++, options_.base_model_hash, database);
            bind_text(statement, parameter, version, database);
            if (sqlite3_step(statement.get()) == SQLITE_ROW) {
                AdapterRecord adapter;
                adapter.version = column_text(statement.get(), 0);
                adapter.base_model_hash = column_text(statement.get(), 1);
                adapter.dataset_snapshot_id = column_text(statement.get(), 2);
                adapter.sha256 = column_text(statement.get(), 3);
                adapter.created_at_unix_seconds = sqlite3_column_int64(statement.get(), 4);
                adapter.active = sqlite3_column_int(statement.get(), 5) != 0;
                result.adapter = std::move(adapter);
            }
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    AdapterLookupResult latest_adapter(sqlite3* database) {
        AdapterLookupResult result;
        try {
            const char* sql = options_.base_model_hash.empty()
                                  ? "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active "
                                    "FROM adapters ORDER BY rowid DESC LIMIT 1"
                                  : "SELECT version, base_model_hash, dataset_snapshot_id, sha256, created_at, active "
                                    "FROM adapters WHERE base_model_hash = ? ORDER BY rowid DESC LIMIT 1";
            Statement statement(database, sql);
            if (!options_.base_model_hash.empty()) bind_text(statement, 1, options_.base_model_hash, database);
            if (sqlite3_step(statement.get()) == SQLITE_ROW) {
                AdapterRecord adapter;
                adapter.version = column_text(statement.get(), 0);
                adapter.base_model_hash = column_text(statement.get(), 1);
                adapter.dataset_snapshot_id = column_text(statement.get(), 2);
                adapter.sha256 = column_text(statement.get(), 3);
                adapter.created_at_unix_seconds = sqlite3_column_int64(statement.get(), 4);
                adapter.active = sqlite3_column_int(statement.get(), 5) != 0;
                result.adapter = std::move(adapter);
            }
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    AdapterFeedbackStatsResult adapter_feedback_stats(sqlite3* database, const std::string& version) {
        AdapterFeedbackStatsResult result;
        try {
            if (version.size() > kMaxMetadataBytes || !is_valid_utf8(version)) throw std::runtime_error("invalid adapter version");
            const char* sql = options_.base_model_hash.empty()
                                  ? "SELECT COALESCE(SUM(target_characters), 0), COALESCE(SUM(CASE WHEN signal_type = 1 "
                                    "THEN correction_characters ELSE 0 END), 0) FROM samples WHERE adapter_version = ?"
                                  : "SELECT COALESCE(SUM(target_characters), 0), COALESCE(SUM(CASE WHEN signal_type = 1 "
                                    "THEN correction_characters ELSE 0 END), 0) FROM samples WHERE adapter_version = ? AND base_model_hash = ?";
            Statement statement(database, sql);
            bind_text(statement, 1, version, database);
            if (!options_.base_model_hash.empty()) bind_text(statement, 2, options_.base_model_hash, database);
            if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "read adapter feedback statistics"));
            result.eligible_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 0), "adapter target count");
            result.correction_target_characters = checked_u64(sqlite3_column_int64(statement.get(), 1), "adapter correction count");
            result.operation = operation_success();
        } catch (const std::exception& error) {
            result.operation = operation_failure(error.what());
        }
        return result;
    }

    StoreOperationResult activate_adapter(sqlite3* database, const std::string& version) {
        try {
            if (version.size() > kMaxEventIdBytes || !is_valid_utf8(version)) return operation_failure("invalid adapter version");
            Transaction transaction(database, "BEGIN IMMEDIATE");
            if (!version.empty()) {
                Statement existing(database, "SELECT 1 FROM adapters WHERE version = ?");
                bind_text(existing, 1, version, database);
                if (sqlite3_step(existing.get()) != SQLITE_ROW) return operation_failure("adapter does not exist");
            }
            execute(database, "UPDATE adapters SET active = 0 WHERE active = 1", "deactivate current adapter");
            if (!version.empty()) {
                Statement activate(database, "UPDATE adapters SET active = 1 WHERE version = ?");
                bind_text(activate, 1, version, database);
                step_done(activate, database, "activate rollback adapter");
                if (sqlite3_changes(database) != 1) throw std::runtime_error("activate rollback adapter changed the wrong row count");
            }
            transaction.commit();
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

    StoreOperationResult deactivate_adapter(sqlite3* database, const std::string& version) {
        try {
            if (version.empty() || version.size() > kMaxEventIdBytes || !is_valid_utf8(version)) {
                return operation_failure("invalid adapter version");
            }
            Statement statement(database, "UPDATE adapters SET active = 0 WHERE version = ? AND active = 1");
            bind_text(statement, 1, version, database);
            step_done(statement, database, "deactivate adapter");
            return operation_success();
        } catch (const std::exception& error) {
            return operation_failure(error.what());
        }
    }

private:
    StoreOperationResult initialize_database() {
        if (!initialization_error_.empty()) return operation_failure(initialization_error_);
        try {
            validate_private_directory(data_directory_);
            validate_database_path_before_open(database_path_);
            sqlite3* raw_database = nullptr;
            const int open_result = sqlite3_open_v2(database_path_.string().c_str(), &raw_database,
                                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
            if (open_result != SQLITE_OK) {
                const auto error = sqlite_error(raw_database, "open feedback database");
                if (raw_database != nullptr) sqlite3_close(raw_database);
                return operation_failure(error);
            }
            database_ = raw_database;
            validate_private_database(database_path_);
            sqlite3_extended_result_codes(database_, 1);
            const auto timeout = options_.busy_timeout.count();
            const int timeout_milliseconds = timeout <= 0 ? 0 : timeout > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                                                               : static_cast<int>(timeout);
            require_sqlite(sqlite3_busy_timeout(database_, timeout_milliseconds), database_, "configure SQLite busy timeout");
            execute(database_, "PRAGMA foreign_keys = ON", "enable SQLite foreign keys");
            execute(database_, "PRAGMA journal_mode = WAL", "enable SQLite WAL mode");
            execute(database_, "PRAGMA synchronous = NORMAL", "configure SQLite synchronous mode");
            execute(database_, "PRAGMA secure_delete = ON", "enable SQLite secure deletion");
            migrate(database_);
            Statement state(database_, "SELECT learning_enabled FROM learning_state WHERE id = 1");
            if (sqlite3_step(state.get()) != SQLITE_ROW) throw std::runtime_error("feedback learning state is missing");
            learning_enabled_.store(sqlite3_column_int(state.get(), 0) != 0, std::memory_order_release);
            return operation_success();
        } catch (const std::exception& error) {
            if (database_ != nullptr) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            return operation_failure(error.what());
        }
    }

    void migrate(sqlite3* database) {
        int version = 0;
        {
            Statement statement(database, "PRAGMA user_version");
            if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "read SQLite schema version"));
            version = sqlite3_column_int(statement.get(), 0);
        }
        if (version > kSchemaVersion) throw std::runtime_error("feedback database schema is newer than this service");
        if (version == kSchemaVersion) return;

        Transaction transaction(database, "BEGIN IMMEDIATE");
        if (version < 1) {
            execute(database, "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
                    "create schema migrations");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS samples ("
                    "event_id TEXT PRIMARY KEY, left_context TEXT NOT NULL, bopomofo_sequence TEXT NOT NULL, "
                    "committed_characters TEXT NOT NULL, predicted_top1 TEXT NOT NULL, manually_chosen_flags BLOB NOT NULL, "
                    "signal_type INTEGER NOT NULL, base_model_hash TEXT NOT NULL, adapter_version TEXT NOT NULL, "
                    "created_at INTEGER NOT NULL, target_characters INTEGER NOT NULL CHECK(target_characters >= 0), "
                    "validation_member INTEGER NOT NULL CHECK(validation_member IN (0, 1)))",
                    "create feedback samples");
            execute(database, "CREATE INDEX IF NOT EXISTS samples_retention_index ON samples(created_at, event_id)", "index feedback samples");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS dataset_snapshots ("
                    "snapshot_id TEXT PRIMARY KEY, sha256 TEXT NOT NULL, created_at INTEGER NOT NULL, "
                    "total_samples INTEGER NOT NULL, total_target_characters INTEGER NOT NULL, "
                    "training_target_characters INTEGER NOT NULL, validation_target_characters INTEGER NOT NULL, "
                    "validation_samples INTEGER NOT NULL)",
                    "create dataset snapshots");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS dataset_snapshot_samples ("
                    "snapshot_id TEXT NOT NULL REFERENCES dataset_snapshots(snapshot_id) ON DELETE CASCADE, "
                    "ordinal INTEGER NOT NULL, event_id TEXT NOT NULL, left_context TEXT NOT NULL, bopomofo_sequence TEXT NOT NULL, "
                    "committed_characters TEXT NOT NULL, predicted_top1 TEXT NOT NULL, manually_chosen_flags BLOB NOT NULL, "
                    "signal_type INTEGER NOT NULL, base_model_hash TEXT NOT NULL, adapter_version TEXT NOT NULL, "
                    "created_at INTEGER NOT NULL, target_characters INTEGER NOT NULL, validation_member INTEGER NOT NULL, "
                    "PRIMARY KEY(snapshot_id, ordinal))",
                    "create immutable snapshot samples");
            execute(database, "CREATE INDEX IF NOT EXISTS snapshot_sample_event_index ON dataset_snapshot_samples(event_id)",
                    "index immutable snapshot samples");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS training_runs ("
                    "run_id TEXT PRIMARY KEY, snapshot_id TEXT NOT NULL, kind INTEGER NOT NULL, started_at INTEGER NOT NULL, "
                    "completed_at INTEGER, succeeded INTEGER CHECK(succeeded IN (0, 1)), "
                    "eligible_target_characters INTEGER NOT NULL, eligible_samples INTEGER NOT NULL)",
                    "create training runs");
            execute(database, "CREATE INDEX IF NOT EXISTS training_runs_completed_index ON training_runs(completed_at)", "index training runs");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS adapters ("
                    "version TEXT PRIMARY KEY, base_model_hash TEXT NOT NULL, dataset_snapshot_id TEXT NOT NULL, "
                    "sha256 TEXT NOT NULL, created_at INTEGER NOT NULL, active INTEGER NOT NULL CHECK(active IN (0, 1)))",
                    "create adapter records");
            execute(database, "CREATE UNIQUE INDEX IF NOT EXISTS one_active_adapter ON adapters(active) WHERE active = 1", "index active adapter");
            execute(database,
                    "CREATE TABLE IF NOT EXISTS learning_state ("
                    "id INTEGER PRIMARY KEY CHECK(id = 1), learning_enabled INTEGER NOT NULL CHECK(learning_enabled IN (0, 1)))",
                    "create learning state");
            execute(database, "INSERT OR IGNORE INTO learning_state(id, learning_enabled) VALUES(1, 0)", "initialize learning state");
            Statement migration(database, "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, ?)");
            bind_i64(migration, 1, unix_seconds(), database);
            step_done(migration, database, "record schema migration");
            execute(database, "PRAGMA user_version = 1", "set SQLite schema version");
        }
        if (version < 2) {
            execute(database,
                    "ALTER TABLE samples ADD COLUMN correction_characters INTEGER NOT NULL DEFAULT 0 "
                    "CHECK(correction_characters >= 0 AND correction_characters <= target_characters)",
                    "add exact correction character count");
            Statement migration(database, "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(2, ?)");
            bind_i64(migration, 1, unix_seconds(), database);
            step_done(migration, database, "record schema migration");
            execute(database, "PRAGMA user_version = 2", "set SQLite schema version");
        }
        transaction.commit();
    }

    bool validate_event(FeedbackEvent& event, std::string& reason) const {
        const auto& eligibility = event.eligibility;
        if (!eligibility.approved || eligibility.sensitive_context || eligibility.cancelled || !eligibility.complete_bopomofo ||
            !eligibility.one_to_one_alignment || !eligibility.targets_are_legal || eligibility.excessive_unknown_tokens ||
            eligibility.pathological_input) {
            reason = "feedback is not eligible for personal learning";
            return false;
        }
        std::size_t target_characters = 0;
        std::size_t predicted_characters = 0;
        if (event.event_id.empty() || event.event_id.size() > kMaxEventIdBytes || event.left_context.size() > options_.max_context_bytes ||
            event.bopomofo_sequence.empty() || event.bopomofo_sequence.size() > options_.max_bopomofo_bytes ||
            event.committed_characters.empty() || event.committed_characters.size() > options_.max_target_bytes ||
            event.predicted_top1.size() > options_.max_target_bytes || event.base_model_hash.size() > kMaxMetadataBytes ||
            event.adapter_version.size() > kMaxMetadataBytes || !is_valid_utf8(event.event_id) || !is_valid_utf8(event.left_context) ||
            !is_valid_utf8(event.bopomofo_sequence) || !is_valid_utf8(event.committed_characters, &target_characters) ||
            !is_valid_utf8(event.predicted_top1, &predicted_characters) || !is_valid_utf8(event.base_model_hash) ||
            !is_valid_utf8(event.adapter_version)) {
            reason = "feedback contains invalid or oversized text";
            return false;
        }
        if (target_characters == 0 || target_characters != predicted_characters ||
            target_characters > options_.max_target_characters ||
            event.manually_chosen_flags.size() != target_characters ||
            !std::all_of(event.manually_chosen_flags.begin(), event.manually_chosen_flags.end(),
                         [](std::uint8_t value) { return value <= 1U; }) ||
            !valid_signal(event.signal_type) || event.created_at_unix_seconds < 0) {
            reason = "feedback fields are inconsistent";
            return false;
        }
        if (!options_.base_model_hash.empty() && event.base_model_hash != options_.base_model_hash) {
            reason = "feedback belongs to a different base model";
            return false;
        }
        event.created_at_unix_seconds = unix_seconds();
        return true;
    }

    bool insert_event(sqlite3* database, const FeedbackEvent& event) {
        Statement statement(database,
                            "INSERT INTO samples (event_id, left_context, bopomofo_sequence, committed_characters, predicted_top1, "
                            "manually_chosen_flags, signal_type, base_model_hash, adapter_version, created_at, target_characters, "
                            "validation_member, correction_characters) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                            "ON CONFLICT(event_id) DO NOTHING");
        bind_text(statement, 1, event.event_id, database);
        bind_text(statement, 2, event.left_context, database);
        bind_text(statement, 3, event.bopomofo_sequence, database);
        bind_text(statement, 4, event.committed_characters, database);
        bind_text(statement, 5, event.predicted_top1, database);
        bind_blob(statement, 6, event.manually_chosen_flags, database);
        bind_i64(statement, 7, static_cast<std::int64_t>(event.signal_type), database);
        bind_text(statement, 8, event.base_model_hash, database);
        bind_text(statement, 9, event.adapter_version, database);
        bind_i64(statement, 10, event.created_at_unix_seconds, database);
        std::size_t targets = 0;
        (void)is_valid_utf8(event.committed_characters, &targets);
        bind_i64(statement, 11, static_cast<std::int64_t>(targets), database);
        require_sqlite(sqlite3_bind_int(statement.get(), 12,
                                        FeedbackStore::deterministic_validation_member(event.event_id) ? 1 : 0),
                       database, "bind validation membership");
        bind_i64(statement, 13, static_cast<std::int64_t>(correction_character_count(event)), database);
        step_done(statement, database, "store feedback event");
        return sqlite3_changes(database) != 0;
    }

    RetentionResult apply_retention_impl(sqlite3* database) {
        RetentionResult result;
        Transaction transaction(database, "BEGIN IMMEDIATE");
        execute(database, "CREATE TEMP TABLE IF NOT EXISTS retention_event_ids(event_id TEXT PRIMARY KEY, target_characters INTEGER NOT NULL)",
                "create retention scratch table");
        execute(database, "DELETE FROM retention_event_ids", "clear retention scratch table");

        const auto now = unix_seconds();
        const auto maximum_age = options_.retention.max_age.count();
        std::int64_t cutoff = now;
        if (maximum_age > 0) {
            if (maximum_age > std::numeric_limits<std::int64_t>::max() / 3600) {
                cutoff = std::numeric_limits<std::int64_t>::min();
            } else {
                const auto age_seconds = maximum_age * 3600;
                cutoff = now < std::numeric_limits<std::int64_t>::min() + age_seconds
                             ? std::numeric_limits<std::int64_t>::min()
                             : now - age_seconds;
            }
        }
        std::uint64_t retained_characters = 0;
        std::vector<std::pair<std::string, std::uint64_t>> to_remove;
        Statement source(database, "SELECT event_id, target_characters, created_at FROM samples ORDER BY created_at ASC, event_id ASC");
        while (sqlite3_step(source.get()) == SQLITE_ROW) {
            const auto event_id = column_text(source.get(), 0);
            const auto targets = checked_u64(sqlite3_column_int64(source.get(), 1), "target character count");
            const auto created_at = sqlite3_column_int64(source.get(), 2);
            if (created_at < cutoff) {
                to_remove.emplace_back(event_id, targets);
            } else {
                if (retained_characters > std::numeric_limits<std::uint64_t>::max() - targets) {
                    throw std::runtime_error("retention target character count overflow");
                }
                retained_characters += targets;
            }
        }
        if (sqlite3_errcode(database) != SQLITE_DONE) throw std::runtime_error(sqlite_error(database, "read retention candidates"));

        if (retained_characters > options_.retention.max_target_characters) {
            Statement candidates(database, "SELECT event_id, target_characters FROM samples WHERE created_at >= ? ORDER BY created_at ASC, event_id ASC");
            bind_i64(candidates, 1, cutoff, database);
            while (retained_characters > options_.retention.max_target_characters && sqlite3_step(candidates.get()) == SQLITE_ROW) {
                const auto event_id = column_text(candidates.get(), 0);
                const auto targets = checked_u64(sqlite3_column_int64(candidates.get(), 1), "target character count");
                to_remove.emplace_back(event_id, targets);
                retained_characters -= std::min(retained_characters, targets);
            }
        }

        Statement insert(database, "INSERT OR IGNORE INTO retention_event_ids(event_id, target_characters) VALUES(?, ?)");
        for (const auto& [event_id, targets] : to_remove) {
            require_sqlite(sqlite3_reset(insert.get()), database, "reset retention statement");
            require_sqlite(sqlite3_clear_bindings(insert.get()), database, "clear retention bindings");
            bind_text(insert, 1, event_id, database);
            bind_i64(insert, 2, static_cast<std::int64_t>(targets), database);
            step_done(insert, database, "record retention removal");
        }
        {
            Statement count(database,
                            "SELECT COUNT(*) FROM dataset_snapshots WHERE EXISTS ("
                            "SELECT 1 FROM dataset_snapshot_samples AS snapshot_sample "
                            "JOIN retention_event_ids AS removed ON removed.event_id = snapshot_sample.event_id "
                            "WHERE snapshot_sample.snapshot_id = dataset_snapshots.snapshot_id) "
                            "AND NOT EXISTS (SELECT 1 FROM training_runs WHERE training_runs.snapshot_id = dataset_snapshots.snapshot_id "
                            "AND training_runs.completed_at IS NULL)");
            if (sqlite3_step(count.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "count invalidated snapshots"));
            result.invalidated_snapshots = checked_u64(sqlite3_column_int64(count.get(), 0), "invalidated snapshot count");
        }
        execute(database,
                "DELETE FROM dataset_snapshots WHERE EXISTS ("
                "SELECT 1 FROM dataset_snapshot_samples AS snapshot_sample "
                "JOIN retention_event_ids AS removed ON removed.event_id = snapshot_sample.event_id "
                "WHERE snapshot_sample.snapshot_id = dataset_snapshots.snapshot_id) "
                "AND NOT EXISTS (SELECT 1 FROM training_runs WHERE training_runs.snapshot_id = dataset_snapshots.snapshot_id "
                "AND training_runs.completed_at IS NULL)",
                "delete invalidated snapshots");
        execute(database, "DELETE FROM samples WHERE event_id IN (SELECT event_id FROM retention_event_ids)", "delete retained feedback samples");
        {
            Statement count(database, "SELECT COUNT(*), COALESCE(SUM(target_characters), 0) FROM retention_event_ids");
            if (sqlite3_step(count.get()) != SQLITE_ROW) throw std::runtime_error(sqlite_error(database, "count retention removals"));
            result.removed_samples = checked_u64(sqlite3_column_int64(count.get(), 0), "removed sample count");
            result.removed_target_characters = checked_u64(sqlite3_column_int64(count.get(), 1), "removed target count");
        }
        transaction.commit();
        result.operation = operation_success();
        return result;
    }

    void writer_loop() {
        for (;;) {
            std::function<void(sqlite3*)> work;
            {
                std::unique_lock lock(queue_mutex_);
                queue_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) break;
                    continue;
                }
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            queue_space_.notify_all();
            work(database_);
        }
        if (database_ != nullptr) {
            sqlite3_close(database_);
            database_ = nullptr;
        }
    }

    FeedbackStoreOptions options_;
    std::filesystem::path data_directory_;
    std::filesystem::path database_path_;
    sqlite3* database_ = nullptr;
    std::string initialization_error_;
    std::atomic<bool> available_{false};
    std::atomic<bool> learning_enabled_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable queue_space_;
    std::deque<std::function<void(sqlite3*)>> queue_;
    bool stopping_ = false;
    std::thread writer_;
    std::uint64_t successful_event_writes_ = 0;
};

FeedbackStore::FeedbackStore(FeedbackStoreOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

FeedbackStore::~FeedbackStore() = default;

FeedbackEnqueueResult FeedbackStore::enqueue(FeedbackEvent event) { return impl_->enqueue(std::move(event)); }

std::future<StoreOperationResult> FeedbackStore::set_learning_enabled(bool enabled) {
    if (!enabled) impl_->disable_learning_in_memory();
    return impl_->submit<StoreOperationResult>([this, enabled](sqlite3* database) { return impl_->set_learning_enabled(database, enabled); });
}

std::future<StoreOperationResult> FeedbackStore::delete_all_personal_data() {
    impl_->disable_learning_in_memory();
    return impl_->submit<StoreOperationResult>([this](sqlite3* database) { return impl_->delete_all_personal_data(database); },
                                               true, false);
}

std::future<RetentionResult> FeedbackStore::apply_retention() {
    return impl_->submit<RetentionResult>([this](sqlite3* database) { return impl_->apply_retention(database); });
}

std::future<SnapshotResult> FeedbackStore::create_dataset_snapshot(SnapshotOptions options) {
    return impl_->submit<SnapshotResult>([this, options](sqlite3* database) { return impl_->create_snapshot(database, options); });
}

std::future<SnapshotLoadResult> FeedbackStore::load_dataset_snapshot(std::string snapshot_id) {
    return impl_->submit<SnapshotLoadResult>([this, snapshot_id = std::move(snapshot_id)](sqlite3* database) {
        return impl_->load_snapshot(database, snapshot_id);
    });
}

std::future<TrainingAccounting> FeedbackStore::training_accounting() {
    return impl_->submit<TrainingAccounting>([this](sqlite3* database) { return impl_->accounting(database); });
}

std::future<StoreOperationResult> FeedbackStore::record_training_started(TrainingRunStart run) {
    return impl_->submit<StoreOperationResult>([this, run = std::move(run)](sqlite3* database) mutable {
        return impl_->record_training_started(database, std::move(run));
    });
}

std::future<IncompleteTrainingRunsResult> FeedbackStore::incomplete_training_runs() {
    return impl_->submit<IncompleteTrainingRunsResult>(
        [this](sqlite3* database) { return impl_->incomplete_training_runs(database); }, true);
}

std::future<StoreOperationResult> FeedbackStore::record_training_finished(std::string run_id, bool succeeded,
                                                                            std::int64_t finished_at_unix_seconds) {
    return impl_->submit<StoreOperationResult>([this, run_id = std::move(run_id), succeeded, finished_at_unix_seconds](sqlite3* database) {
        return impl_->record_training_finished(database, run_id, succeeded, finished_at_unix_seconds);
    });
}

std::future<StoreOperationResult> FeedbackStore::record_adapter(AdapterRecord adapter) {
    return impl_->submit<StoreOperationResult>([this, adapter = std::move(adapter)](sqlite3* database) {
        return impl_->record_adapter(database, adapter);
    });
}

std::future<StoreOperationResult> FeedbackStore::record_adapter_and_finish_training(AdapterRecord adapter,
                                                                                     std::string run_id) {
    return impl_->submit<StoreOperationResult>(
        [this, adapter = std::move(adapter), run_id = std::move(run_id)](sqlite3* database) {
            return impl_->record_adapter(database, adapter, &run_id);
        },
        true);
}

std::future<AdapterLookupResult> FeedbackStore::active_adapter() {
    return impl_->submit<AdapterLookupResult>([this](sqlite3* database) { return impl_->active_adapter(database); }, true);
}

std::future<AdapterLookupResult> FeedbackStore::latest_adapter() {
    return impl_->submit<AdapterLookupResult>([this](sqlite3* database) { return impl_->latest_adapter(database); }, true);
}

std::future<AdapterLookupResult> FeedbackStore::previous_adapter(std::string version) {
    return impl_->submit<AdapterLookupResult>(
        [this, version = std::move(version)](sqlite3* database) { return impl_->previous_adapter(database, version); }, true);
}

std::future<AdapterFeedbackStatsResult> FeedbackStore::adapter_feedback_stats(std::string version) {
    return impl_->submit<AdapterFeedbackStatsResult>(
        [this, version = std::move(version)](sqlite3* database) { return impl_->adapter_feedback_stats(database, version); }, true);
}

std::future<StoreOperationResult> FeedbackStore::activate_adapter(std::string version) {
    return impl_->submit<StoreOperationResult>(
        [this, version = std::move(version)](sqlite3* database) { return impl_->activate_adapter(database, version); }, true);
}

std::future<StoreOperationResult> FeedbackStore::deactivate_adapter(std::string version) {
    return impl_->submit<StoreOperationResult>(
        [this, version = std::move(version)](sqlite3* database) { return impl_->deactivate_adapter(database, version); }, true);
}

std::future<StoreOperationResult> FeedbackStore::flush() {
    return impl_->submit<StoreOperationResult>([](sqlite3*) { return operation_success(); }, true);
}

bool FeedbackStore::learning_enabled() const noexcept { return impl_->learning_enabled(); }

bool FeedbackStore::available() const noexcept { return impl_->available(); }

const std::filesystem::path& FeedbackStore::data_directory() const noexcept { return impl_->data_directory(); }

const std::filesystem::path& FeedbackStore::database_path() const noexcept { return impl_->database_path(); }

bool FeedbackStore::deterministic_validation_member(const std::string& event_id) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : event_id) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash % 10U == 0U;
}

std::filesystem::path FeedbackStore::default_data_directory() {
#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data != nullptr && local_app_data[0] != '\0') {
        return std::filesystem::path(local_app_data) / "LlavonIme";
    }
#else
    if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME"); xdg_data_home != nullptr && xdg_data_home[0] != '\0') {
        return std::filesystem::path(xdg_data_home) / "llavon-ime";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "llavon-ime";
    }
#endif
    std::error_code error;
    auto temporary = std::filesystem::temp_directory_path(error);
    if (error) temporary = ".";
#ifndef _WIN32
    return temporary / ("llavon-ime-" + std::to_string(static_cast<unsigned long long>(::getuid())));
#else
    return temporary / "llavon-ime";
#endif
}

}  // namespace imesvc::training
