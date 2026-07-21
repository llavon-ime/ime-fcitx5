#ifndef _WIN32

#include "unix_socket_server.hpp"

#include "../pipe/protocol.hpp"
#include "../training/feedback_store.hpp"
#include "../training/adapter_publisher.hpp"
#include "../training/system_environment.hpp"
#include "../training/training_orchestrator.hpp"
#include "../training/sha256.hpp"

#ifndef IMESVC_HAS_LLAMA
#define IMESVC_HAS_LLAMA 0
#endif

#if IMESVC_HAS_LLAMA
#include "../engine/llamaEngine.hpp"
#endif

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <queue>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef __APPLE__
#include <sys/types.h>
#endif

namespace imesvc {

namespace {

std::uint32_t read_u32_le(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool read_all(int fd, void* destination, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count == 0) return false;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

void write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto count = ::send(fd, bytes + offset, size - offset, flags);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "send on Unix socket");
        }
        if (count == 0) throw std::runtime_error("Unix socket closed while sending");
        offset += static_cast<std::size_t>(count);
    }
}

bool owned_by_current_user(const struct stat& status) {
    return status.st_uid == ::getuid();
}

struct stat lstat_or_throw(const std::filesystem::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        throw std::system_error(errno, std::generic_category(), "lstat " + path.string());
    }
    return status;
}

void require_private_directory(const std::filesystem::path& path, bool create) {
    if (create) {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) throw std::system_error(error.value(), std::generic_category(), "create runtime directory");
    }
    const auto status = lstat_or_throw(path);
    if (!S_ISDIR(status.st_mode) || !owned_by_current_user(status)) {
        throw std::runtime_error("runtime directory must be an owner-only directory: " + path.string());
    }
    if (::chmod(path.c_str(), S_IRWXU) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod runtime directory");
    }
}

bool socket_is_active(const std::filesystem::path& path) {
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) throw std::system_error(errno, std::generic_category(), "create Unix socket probe");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto string_path = path.string();
    if (string_path.size() >= sizeof(address.sun_path)) {
        ::close(probe);
        throw std::runtime_error("Unix socket path is too long: " + string_path);
    }
    std::memcpy(address.sun_path, string_path.c_str(), string_path.size() + 1);
    const int result = ::connect(probe, reinterpret_cast<const sockaddr*>(&address),
                                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + string_path.size() + 1));
    const int saved_errno = errno;
    ::close(probe);
    if (result == 0) return true;
    if (saved_errno == ECONNREFUSED || saved_errno == ENOENT || saved_errno == ECONNRESET) return false;
    throw std::system_error(saved_errno, std::generic_category(), "probe Unix socket");
}

std::string utf16_to_utf8(const std::u16string& value) {
    std::string result;
    result.reserve(value.size() * 3U);
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t scalar = static_cast<std::uint16_t>(value[index]);
        if (scalar >= 0xd800U && scalar <= 0xdbffU) {
            if (++index >= value.size()) throw std::runtime_error("invalid UTF-16 feedback text");
            const auto low = static_cast<std::uint16_t>(value[index]);
            if (low < 0xdc00U || low > 0xdfffU) throw std::runtime_error("invalid UTF-16 feedback text");
            scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
        } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
            throw std::runtime_error("invalid UTF-16 feedback text");
        }
        if (scalar <= 0x7fU) {
            result.push_back(static_cast<char>(scalar));
        } else if (scalar <= 0x7ffU) {
            result.push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
            result.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
        } else if (scalar <= 0xffffU) {
            result.push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
        } else {
            result.push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
        }
    }
    return result;
}

std::string event_id_string(const protocol::EventId& id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(id.size() * 2U);
    for (const auto byte : id) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
    }
    return result;
}

bool valid_sha256(std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

std::vector<char32_t> utf16_scalars(const std::u16string& value) {
    std::vector<char32_t> result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t scalar = static_cast<std::uint16_t>(value[index]);
        if (scalar >= 0xd800U && scalar <= 0xdbffU) {
            const auto low = static_cast<std::uint16_t>(value[++index]);
            scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
        }
        result.push_back(static_cast<char32_t>(scalar));
    }
    return result;
}

bool legal_feedback_targets(const protocol::FeedbackRequest& request, const SharedModelRuntime& runtime) {
    std::vector<std::u16string> readings;
    std::size_t begin = 0;
    while (begin < request.bopomofo_sequence.size()) {
        const auto end = request.bopomofo_sequence.find(protocol::kBopomofoReadingSeparator, begin);
        readings.push_back(request.bopomofo_sequence.substr(begin, end == std::u16string::npos ? end : end - begin));
        if (end == std::u16string::npos) break;
        begin = end + 1;
    }
    const auto targets = utf16_scalars(request.committed_characters);
    if (readings.size() != targets.size()) return false;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto candidates = runtime.lookup(readings[index]);
        if (std::find(candidates.begin(), candidates.end(), targets[index]) == candidates.end()) return false;
    }
    return true;
}

bool feedback_signal_is_consistent(const protocol::FeedbackRequest& request) {
    const auto committed = utf16_scalars(request.committed_characters);
    const auto predicted = utf16_scalars(request.predicted_top1);
    bool manual = false;
    bool differs = false;
    for (std::size_t index = 0; index < committed.size(); ++index) {
        manual = manual || request.manually_chosen_flags[index];
        differs = differs || committed[index] != predicted[index];
    }
    switch (request.signal_type) {
        case protocol::FeedbackSignal::ExplicitCorrection: return manual && differs;
        case protocol::FeedbackSignal::ExplicitTop1Selection: return manual && !differs;
        case protocol::FeedbackSignal::AcceptedPrediction: return !manual && !differs;
        case protocol::FeedbackSignal::FallbackCommit: return !manual;
    }
    return false;
}

const char* training_state_name(training::TrainingOrchestratorState state) noexcept {
    switch (state) {
        case training::TrainingOrchestratorState::Waiting: return "waiting";
        case training::TrainingOrchestratorState::Checking: return "checking";
        case training::TrainingOrchestratorState::Blocked: return "blocked";
        case training::TrainingOrchestratorState::Launching: return "launching";
        case training::TrainingOrchestratorState::Running: return "running";
        case training::TrainingOrchestratorState::Error: return "error";
        case training::TrainingOrchestratorState::Stopped: return "stopped";
    }
    return "unknown";
}

#if IMESVC_HAS_LLAMA
struct ServingValidationMetrics {
    std::uint64_t targets = 0;
    std::uint64_t top1_correct = 0;
    std::uint64_t top5_correct = 0;
    std::uint64_t correction_targets = 0;
    std::uint64_t correction_top1_correct = 0;
    std::uint64_t compositions = 0;
    std::uint64_t exact_compositions = 0;
    std::uint64_t empty_candidates = 0;
    double p95_latency_milliseconds = 0.0;
};

double ratio(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

training::StoreOperationResult validate_serving_runtime(const std::filesystem::path& adapter_path,
                                                          const std::vector<training::DatasetSample>& samples,
                                                          training::TrainingRunKind kind,
                                                          const std::shared_ptr<SharedModelRuntime>& runtime,
                                                          std::shared_ptr<const AdapterGeneration> baseline_generation,
                                                          const std::filesystem::path& candidate_map_path) {
    try {
        const auto score = [&samples, &runtime](std::shared_ptr<const AdapterGeneration> generation) {
            LlamaEngine engine(runtime, std::move(generation));
            ServingValidationMetrics metrics;
            std::vector<double> latencies;
            for (const auto& sample : samples) {
                if (!sample.validation_member) continue;
                const auto targets = utf8::utf8to32(sample.committed_characters);
                const auto predicted = utf8::utf8to32(sample.predicted_top1);
                std::vector<PaddingEntry> padding;
                std::size_t begin = 0;
                while (begin < sample.bopomofo_sequence.size()) {
                    const auto end = sample.bopomofo_sequence.find('\x1f', begin);
                    const auto bytes = sample.bopomofo_sequence.substr(
                        begin, end == std::string::npos ? std::string::npos : end - begin);
                    padding.push_back(PaddingEntry{false, 0, utf8::utf8to16(bytes)});
                    if (end == std::string::npos) break;
                    begin = end + 1;
                }
                if (padding.size() != targets.size()) throw std::runtime_error("validation sample alignment is invalid");
                const auto started = std::chrono::steady_clock::now();
                const auto predictions = engine.predict(utf8::utf8to16(sample.left_context), padding);
                latencies.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
                if (predictions.size() != targets.size()) throw std::runtime_error("serving validation returned the wrong segment count");
                bool exact = true;
                for (std::size_t index = 0; index < predictions.size(); ++index) {
                    const auto& candidates = predictions[index].candidates;
                    if (std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
                            return !std::isfinite(candidate.second) || candidate.second < 0.0F || candidate.second > 1.0F;
                        })) {
                        throw std::runtime_error("serving validation produced a non-finite candidate probability");
                    }
                    if (candidates.empty()) {
                        ++metrics.empty_candidates;
                        exact = false;
                    } else {
                        if (candidates.front().first == targets[index]) {
                            ++metrics.top1_correct;
                        } else {
                            exact = false;
                        }
                        const auto top5 = std::min<std::size_t>(5, candidates.size());
                        if (std::find_if(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(top5),
                                         [target = targets[index]](const auto& candidate) {
                                             return candidate.first == target;
                                         }) != candidates.begin() + static_cast<std::ptrdiff_t>(top5)) {
                            ++metrics.top5_correct;
                        }
                    }
                    const bool correction = sample.signal_type == training::FeedbackSignal::ExplicitCorrection &&
                                            index < sample.manually_chosen_flags.size() && sample.manually_chosen_flags[index] != 0 &&
                                            index < predicted.size() && predicted[index] != targets[index];
                    if (correction) {
                        ++metrics.correction_targets;
                        if (!candidates.empty() && candidates.front().first == targets[index]) {
                            ++metrics.correction_top1_correct;
                        }
                    }
                    ++metrics.targets;
                }
                ++metrics.compositions;
                if (exact) ++metrics.exact_compositions;
            }
            if (metrics.targets == 0 || latencies.empty()) throw std::runtime_error("serving validation has no held-out targets");
            std::sort(latencies.begin(), latencies.end());
            const auto p95_index = static_cast<std::size_t>(std::ceil(0.95 * latencies.size())) - 1U;
            metrics.p95_latency_milliseconds = latencies[std::min(p95_index, latencies.size() - 1U)];
            return metrics;
        };

        const auto general_baseline_generation = baseline_generation;
        const auto baseline = score(std::move(baseline_generation));
        auto generation = std::make_shared<AdapterGeneration>();
        generation->revision = 1;
        generation->version = "validation";
        generation->path = adapter_path;
        generation->sha256 = "validation";
        const auto adapted = score(std::move(generation));
        if (adapted.targets != baseline.targets || adapted.compositions != baseline.compositions ||
            adapted.empty_candidates != 0) {
            return {false, "trained adapter produced invalid or empty held-out predictions"};
        }
        {
            std::ifstream input(candidate_map_path);
            if (!input) throw std::runtime_error("open general regression candidate map failed");
            const auto candidate_map = nlohmann::json::parse(input);
            if (!candidate_map.is_object() || candidate_map.empty()) {
                throw std::runtime_error("general regression candidate map is invalid");
            }
            std::vector<std::string> readings;
            readings.reserve(candidate_map.size());
            for (const auto& [reading, candidates] : candidate_map.items()) {
                if (candidates.is_array() && !candidates.empty()) readings.push_back(reading);
            }
            std::sort(readings.begin(), readings.end());
            if (readings.empty()) throw std::runtime_error("general regression candidate map has no readings");
            constexpr std::size_t kMaximumGeneralReadings = 512;
            const auto stride = std::max<std::size_t>(1, readings.size() / kMaximumGeneralReadings);
            LlamaEngine baseline_engine(runtime, general_baseline_generation);
            auto adapted_generation = std::make_shared<AdapterGeneration>();
            adapted_generation->revision = 1;
            adapted_generation->version = "general-validation";
            adapted_generation->path = adapter_path;
            adapted_generation->sha256 = "validation";
            LlamaEngine adapted_engine(runtime, std::move(adapted_generation));
            std::uint64_t total = 0;
            std::uint64_t top1_agreement = 0;
            std::uint64_t top5_recall = 0;
            for (std::size_t index = 0; index < readings.size() && total < kMaximumGeneralReadings; index += stride) {
                const std::vector<PaddingEntry> padding{{false, 0, utf8::utf8to16(readings[index])}};
                const auto baseline_prediction = baseline_engine.predict({}, padding);
                const auto adapted_prediction = adapted_engine.predict({}, padding);
                if (baseline_prediction.size() != 1 || adapted_prediction.size() != 1 ||
                    baseline_prediction.front().candidates.empty() || adapted_prediction.front().candidates.empty()) {
                    return {false, "general regression validation produced empty candidates"};
                }
                const auto& adapted_candidates = adapted_prediction.front().candidates;
                if (std::any_of(adapted_candidates.begin(), adapted_candidates.end(), [](const auto& candidate) {
                        return !std::isfinite(candidate.second) || candidate.second < 0.0F || candidate.second > 1.0F;
                    })) {
                    return {false, "general regression validation produced a non-finite probability"};
                }
                const auto baseline_top1 = baseline_prediction.front().candidates.front().first;
                if (adapted_candidates.front().first == baseline_top1) ++top1_agreement;
                const auto top5 = std::min<std::size_t>(5, adapted_candidates.size());
                if (std::find_if(adapted_candidates.begin(), adapted_candidates.begin() + static_cast<std::ptrdiff_t>(top5),
                                 [baseline_top1](const auto& candidate) { return candidate.first == baseline_top1; }) !=
                    adapted_candidates.begin() + static_cast<std::ptrdiff_t>(top5)) {
                    ++top5_recall;
                }
                ++total;
            }
            if (ratio(top1_agreement, total) + 1e-12 < 0.997 || ratio(top5_recall, total) + 1e-12 < 0.999) {
                return {false, "trained adapter failed the general regression candidate-map gate"};
            }
        }
        if (kind == training::TrainingRunKind::ShadowSmoke) {
            if (adapted.top1_correct < baseline.top1_correct || adapted.top5_correct < baseline.top5_correct) {
                return {false, "shadow adapter regressed held-out serving accuracy"};
            }
            return {true, {}};
        }
        if (ratio(adapted.top1_correct, adapted.targets) + 1e-12 <
            ratio(baseline.top1_correct, baseline.targets) + 0.003) {
            return {false, "trained adapter did not improve held-out top-1 by 0.3 percentage points"};
        }
        if (baseline.correction_targets == 0 || adapted.correction_targets != baseline.correction_targets ||
            ratio(adapted.correction_top1_correct, adapted.correction_targets) + 1e-12 <
                ratio(baseline.correction_top1_correct, baseline.correction_targets) + 0.03) {
            return {false, "trained adapter did not improve explicit-correction top-1 by 3 percentage points"};
        }
        if (ratio(adapted.exact_compositions, adapted.compositions) + 0.005 + 1e-12 <
            ratio(baseline.exact_compositions, baseline.compositions)) {
            return {false, "trained adapter regressed whole-composition exact match"};
        }
        if (ratio(adapted.top5_correct, adapted.targets) + 0.001 + 1e-12 <
            ratio(baseline.top5_correct, baseline.targets)) {
            return {false, "trained adapter regressed held-out top-5 recall"};
        }
        if (adapted.p95_latency_milliseconds > baseline.p95_latency_milliseconds * 1.05) {
            return {false, "trained adapter regressed held-out p95 latency by more than 5 percent"};
        }
        return {true, {}};
    } catch (const std::exception& error) {
        return {false, std::string("serving-backend adapter validation failed: ") + error.what()};
    }
}
#endif

training::FeedbackEvent feedback_event_from_request(const protocol::FeedbackRequest& request, bool targets_are_legal,
                                                     const std::string& base_model_hash, const std::string& adapter_version) {
    training::FeedbackEvent event;
    // The server-issued token, not a client-selected ID, determines holdout membership.
    event.event_id = event_id_string(request.feedback_token);
    event.left_context = utf16_to_utf8(request.left_context);
    event.bopomofo_sequence = utf16_to_utf8(request.bopomofo_sequence);
    event.committed_characters = utf16_to_utf8(request.committed_characters);
    event.predicted_top1 = utf16_to_utf8(request.predicted_top1);
    event.manually_chosen_flags.reserve(request.manually_chosen_flags.size());
    for (const bool chosen : request.manually_chosen_flags) event.manually_chosen_flags.push_back(chosen ? 1U : 0U);
    event.signal_type = static_cast<training::FeedbackSignal>(request.signal_type);
    event.base_model_hash = base_model_hash;
    event.adapter_version = adapter_version;
    event.created_at_unix_seconds = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    event.eligibility.sensitive_context = false;
    event.eligibility.cancelled = false;
    event.eligibility.complete_bopomofo = true;
    event.eligibility.one_to_one_alignment = true;
    event.eligibility.targets_are_legal = targets_are_legal;
    event.eligibility.excessive_unknown_tokens = event.left_context.size() > 4096U;
    event.eligibility.pathological_input = event.committed_characters.size() > 2048U;
    event.eligibility.approved = targets_are_legal && !event.eligibility.excessive_unknown_tokens &&
                                 !event.eligibility.pathological_input;
    return event;
}

void prepare_socket_path(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) throw std::runtime_error("Unix socket must have a parent directory");
    require_private_directory(parent, true);

    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) return;
        throw std::system_error(errno, std::generic_category(), "lstat Unix socket");
    }
    if (S_ISLNK(status.st_mode)) throw std::runtime_error("refusing symlink Unix socket path: " + path.string());
    if (!S_ISSOCK(status.st_mode)) throw std::runtime_error("refusing non-socket path: " + path.string());
    if (!owned_by_current_user(status)) throw std::runtime_error("refusing Unix socket owned by another user");
    if (socket_is_active(path)) throw std::runtime_error("Unix socket is already in use: " + path.string());
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "remove stale Unix socket");
    }
}

std::uint64_t peer_uid(int fd) {
#ifdef __linux__
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        throw std::system_error(errno, std::generic_category(), "get Unix peer credentials");
    }
    return static_cast<std::uint64_t>(credentials.uid);
#elif defined(__APPLE__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) {
        throw std::system_error(errno, std::generic_category(), "get Unix peer credentials");
    }
    return static_cast<std::uint64_t>(uid);
#else
    (void)fd;
    return static_cast<std::uint64_t>(::getuid());
#endif
}

}  // namespace

class UnixSocketServer::WorkerPool {
public:
    explicit WorkerPool(std::size_t count) {
        count = std::max<std::size_t>(1, count);
        for (std::size_t i = 0; i < count; ++i) workers_.emplace_back([this]() { run(); });
    }

    ~WorkerPool() {
        shutdown();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    bool enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            queue_.push(std::move(task));
        }
        condition_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                // A second call still joins any threads that have not been joined.
            } else {
                stopping_ = true;
            }
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
    }

private:
    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }
            try {
                task();
            } catch (...) {
                // A connection owns the error response.  Worker exceptions must never
                // terminate the service process.
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

class UnixSocketServer::Connection final : public std::enable_shared_from_this<Connection> {
public:
    Connection(UnixSocketServer& server, int fd, std::uint64_t uid) : server_(server), fd_(fd), uid_(uid) {}

    ~Connection() {
        close();
        finalize_close();
    }

    void run() {
        try {
            while (true) {
                const int descriptor = current_fd();
                if (descriptor < 0) break;
                std::array<std::uint8_t, 4> header{};
                if (!read_all(descriptor, header.data(), header.size())) break;
                const auto payload_length = read_u32_le(header.data());
                if (payload_length > protocol::kMaxFramePayloadBytes) {
                    send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "protocol frame is too large");
                    break;
                }
                protocol::ByteVector frame(header.begin(), header.end());
                protocol::ByteVector payload(payload_length);
                if (!read_all(descriptor, payload.data(), payload.size())) break;
                frame.insert(frame.end(), payload.begin(), payload.end());
                try {
                    dispatch(protocol::decode(frame));
                } catch (const protocol::ProtocolError& error) {
                    send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, error.what());
                } catch (const std::exception& error) {
                    send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, error.what());
                }
            }
        } catch (...) {
        }
        finalize_close();
        finished_.store(true, std::memory_order_release);
    }

    void close() noexcept {
        std::lock_guard lock(write_mutex_);
        if (fd_ < 0 || closing_) return;
        closing_ = true;
        ::shutdown(fd_, SHUT_RDWR);
    }

    bool closed() const noexcept {
        std::lock_guard lock(write_mutex_);
        return fd_ < 0 || closing_;
    }

    std::uint64_t uid() const noexcept {
        return uid_;
    }

    void send(const protocol::Message& message) noexcept {
        try {
            const auto bytes = protocol::encode(message);
            std::lock_guard lock(write_mutex_);
            if (fd_ >= 0 && !closing_) write_all(fd_, bytes.data(), bytes.size());
        } catch (...) {
            close();
        }
    }

private:
    void send_error(protocol::ErrorCode code, const protocol::SessionId& id, std::uint64_t request_id,
                    std::uint64_t revision, std::string message) noexcept {
        send(protocol::Message{protocol::Error{code, id, request_id, revision, std::move(message)}});
    }

    void dispatch(const protocol::Message& message) {
        std::visit(
            [this](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<T, protocol::HelloRequest>) {
                    if (!protocol_negotiated_) {
                        send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0,
                                   "protocol negotiation is required before requests");
                        return;
                    }
                }
                if constexpr (std::is_same_v<T, protocol::HelloRequest>) {
                    if (protocol_negotiated_) {
                        send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "protocol was already negotiated");
                        return;
                    }
                    if (value.minimum_version > protocol::kMaxProtocolVersion ||
                        value.maximum_version < protocol::kMinProtocolVersion) {
                        send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "no compatible protocol version");
                        return;
                    }
                    negotiated_capabilities_ = value.capabilities & server_.supported_capabilities();
                    protocol_negotiated_ = true;
                    send(protocol::Message{protocol::HelloResponse{protocol::kProtocolVersion, negotiated_capabilities_}});
                } else if constexpr (std::is_same_v<T, protocol::OpenSessionRequest>) {
                    if (!server_.workers_->enqueue([self = shared_from_this()]() {
                            const auto result = self->server_.sessions_->open_session(self->uid());
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::PredictRequest>) {
                    const auto request = value;
                    server_.record_prediction_activity();
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            const auto result = self->server_.sessions_->predict(self->uid(), request);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                                   request.buffer_revision, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::CloseSessionRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            const auto result = self->server_.sessions_->close_session(self->uid(), request.session_id);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, 0, 0,
                                   "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::StatusRequest>) {
                    const auto result = server_.sessions_->status(uid_, value.session_id);
                    std::visit([this](const auto& response) { send(protocol::Message{response}); }, result);
                } else if constexpr (std::is_same_v<T, protocol::FeedbackRequest>) {
                    if (!protocol_negotiated_ || !protocol::has_capability(negotiated_capabilities_, protocol::Capability::PersonalFeedback) ||
                        !server_.feedback_store_ || server_.runtime_->config().base_model_sha256.empty()) {
                        send_error(protocol::ErrorCode::Unauthorized, {}, 0, 0, "personal feedback is not available");
                        return;
                    }
                    const bool legal = legal_feedback_targets(value, *server_.runtime_);
                    if (!legal || !feedback_signal_is_consistent(value)) {
                        send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, "feedback does not match the recorded prediction");
                        return;
                    }
                    const auto adapter_version = server_.sessions_->consume_feedback_token(uid_, value);
                    if (!adapter_version) {
                        send_error(protocol::ErrorCode::Unauthorized, value.session_id, 0, 0, "feedback prediction token is invalid or expired");
                        return;
                    }
                    const auto result = server_.feedback_store_->enqueue(
                        feedback_event_from_request(value, true, server_.runtime_->config().base_model_sha256, *adapter_version));
                    if (!result.accepted()) {
                        const auto code = result.status == training::FeedbackEnqueueStatus::QueueFull
                                              ? protocol::ErrorCode::ResourceExhausted
                                              : protocol::ErrorCode::InvalidArgument;
                        send_error(code, {}, 0, 0, "feedback was not accepted");
                        return;
                    }
                    send(protocol::Message{protocol::FeedbackAccepted{value.event_id}});
                } else if constexpr (std::is_same_v<T, protocol::TrainingStatusRequest>) {
                    if (!protocol_negotiated_ || !protocol::has_capability(negotiated_capabilities_, protocol::Capability::TrainingStatus) ||
                        !server_.feedback_store_) {
                        send_error(protocol::ErrorCode::Unauthorized, {}, 0, 0, "training status is not available");
                        return;
                    }
                    protocol::TrainingStatusResponse response;
                    response.collecting = server_.feedback_store_->learning_enabled();
                    const auto accounting = server_.feedback_store_->training_accounting().get();
                    response.accepted_feedback_count = accounting.eligible_samples;
                    response.eligible_character_count = accounting.eligible_target_characters;
                    response.active_adapter_version = server_.runtime_->active_adapter_version();
                    response.state = "disabled";
                    response.message = server_.training_configuration_error_.empty()
                                           ? "automatic LoRA training is disabled"
                                           : server_.training_configuration_error_;
                    {
                        std::lock_guard lock(server_.training_mutex_);
                        if (server_.training_orchestrator_) {
                            const auto status = server_.training_orchestrator_->status();
                            response.training = status.state == training::TrainingOrchestratorState::Checking ||
                                                status.state == training::TrainingOrchestratorState::Launching ||
                                                status.state == training::TrainingOrchestratorState::Running;
                            response.state = training_state_name(status.state);
                            response.message = status.reason;
                        }
                    }
                    send(protocol::Message{std::move(response)});
                } else if constexpr (std::is_same_v<T, protocol::DeletePersonalDataRequest>) {
                    if (!protocol_negotiated_ || !protocol::has_capability(negotiated_capabilities_, protocol::Capability::DeletePersonalData) ||
                        uid_ != static_cast<std::uint64_t>(::getuid())) {
                        send_error(protocol::ErrorCode::Unauthorized, {}, 0, 0, "personal-data deletion is not available");
                        return;
                    }
                    if (!server_.workers_->enqueue([self = shared_from_this()]() {
                            const auto result = self->server_.delete_personal_data();
                            if (!result.succeeded) {
                                self->send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, result.error);
                                return;
                            }
                            self->send(protocol::Message{protocol::DeletePersonalDataResponse{true}});
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::ShutdownRequest>) {
                    send(protocol::Message{protocol::ShutdownResponse{true}});
                    server_.request_stop();
                } else {
                    send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "unexpected response message from client");
                }
            },
            message);
    }

    int current_fd() const noexcept {
        std::lock_guard lock(write_mutex_);
        return closing_ ? -1 : fd_;
    }

    void finalize_close() noexcept {
        std::lock_guard lock(write_mutex_);
        if (fd_ < 0) return;
        ::close(fd_);
        fd_ = -1;
        closing_ = true;
    }

    UnixSocketServer& server_;
    int fd_ = -1;
    std::uint64_t uid_ = 0;
    std::atomic_bool finished_{false};
    mutable std::mutex write_mutex_;
    bool closing_ = false;
    bool protocol_negotiated_ = false;
    protocol::Capabilities negotiated_capabilities_ = 0;

public:
    bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
};

UnixSocketServer::UnixSocketServer(UnixServerOptions options) : options_(std::move(options)) {
    socket_path_ = options_.socket_path.value_or(default_socket_path());
    pid_path_ = options_.pid_path.value_or(socket_path_.parent_path() / "service.pid");
    if (socket_path_.is_relative()) socket_path_ = std::filesystem::absolute(socket_path_);
    if (pid_path_.is_relative()) pid_path_ = std::filesystem::absolute(pid_path_);
    runtime_ = std::make_shared<SharedModelRuntime>(options_.runtime);
}

UnixSocketServer::~UnixSocketServer() {
    request_stop();
    close_connections();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    cleanup_endpoint();
}

const char* UnixSocketServer::name() const {
    return "unix-socket";
}

std::filesystem::path UnixSocketServer::default_socket_path() {
    if (const char* override_path = std::getenv("IME_FCITX5_SOCKET_PATH"); override_path && override_path[0] != '\0') {
        return override_path;
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = std::getenv("TMPDIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = "/tmp";
    return std::filesystem::path(runtime) / "llavon-ime" / "ime.sock";
}

std::filesystem::path UnixSocketServer::default_pid_path() {
    const auto socket = default_socket_path();
    return socket.parent_path() / "service.pid";
}

int UnixSocketServer::run() {
    runtime_->validate_configuration();
    (void)runtime_->lookup(u"");
    sessions_ = std::make_unique<SessionManager>(runtime_, options_.limits);
    const auto feedback_directory = options_.training_data_directory.value_or(training::FeedbackStore::default_data_directory());
    std::error_code feedback_directory_error;
    const bool feedback_directory_exists = std::filesystem::exists(feedback_directory, feedback_directory_error) && !feedback_directory_error;
    if (options_.personal_learning_enabled || options_.lora_training.enabled || feedback_directory_exists) {
        training::FeedbackStoreOptions store_options;
        store_options.data_directory = feedback_directory;
        store_options.base_model_hash = options_.runtime.base_model_sha256;
        try {
            feedback_store_ = std::make_unique<training::FeedbackStore>(std::move(store_options));
            runtime_->set_adapter_failure_handler([this](std::string version) {
                if (feedback_store_) (void)feedback_store_->deactivate_adapter(std::move(version)).get();
            });
            const auto enabled = feedback_store_->set_learning_enabled(options_.personal_learning_enabled).get();
            if (!enabled.succeeded) {
                training_configuration_error_ = "personal learning is unavailable: " + enabled.error;
                std::clog << "[SRV] " << training_configuration_error_ << '\n';
            }
        } catch (const std::exception& error) {
            training_configuration_error_ = std::string("personal learning is unavailable: ") + error.what();
            std::clog << "[SRV] " << training_configuration_error_ << '\n';
            feedback_store_.reset();
        }
    }
    workers_ = std::make_unique<WorkerPool>(std::max<std::size_t>(2, options_.limits.max_concurrent_predictions + 1));

    prepare_socket_path(socket_path_);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "create Unix listening socket");
    listen_fd_ = fd;
#ifdef FD_CLOEXEC
    (void)::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
#endif

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto string_path = socket_path_.string();
    if (string_path.size() >= sizeof(address.sun_path)) throw std::runtime_error("Unix socket path is too long");
    std::memcpy(address.sun_path, string_path.c_str(), string_path.size() + 1);
    const auto address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + string_path.size() + 1);
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        throw std::system_error(errno, std::generic_category(), "bind Unix socket");
    }
    endpoint_owned_ = true;
    if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod Unix socket");
    }
    if (::listen(listen_fd_, 32) != 0) throw std::system_error(errno, std::generic_category(), "listen Unix socket");
    initialize_training();

    if (!pid_path_.parent_path().empty()) require_private_directory(pid_path_.parent_path(), true);
    {
        struct stat status {};
        if (::lstat(pid_path_.c_str(), &status) == 0) {
            if (S_ISLNK(status.st_mode) || !S_ISREG(status.st_mode) || !owned_by_current_user(status)) {
                throw std::runtime_error("refusing unsafe service PID path: " + pid_path_.string());
            }
            if (::unlink(pid_path_.c_str()) != 0) throw std::system_error(errno, std::generic_category(), "remove stale PID file");
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "lstat service PID file");
        }
        std::ofstream pid(pid_path_);
        if (!pid) throw std::runtime_error("failed to create service PID file: " + pid_path_.string());
        pid << ::getpid() << '\n';
        if (!pid) throw std::runtime_error("failed to write service PID file: " + pid_path_.string());
        ::chmod(pid_path_.c_str(), S_IRUSR | S_IWUSR);
        pid_owned_ = true;
    }

    std::clog << "[SRV] listening on " << socket_path_ << " epoch=";
    for (const auto byte : sessions_->service_epoch()) std::clog << std::hex << static_cast<unsigned>(byte);
    std::clog << std::dec << '\n';

    while (!stopping_.load(std::memory_order_acquire)) {
        pollfd descriptor{listen_fd_, POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 250);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll Unix listening socket");
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) accept_connections();
        reap_connections();
        sessions_->reap();
        const auto now = std::chrono::steady_clock::now();
        if (adapter_publisher_ &&
            (last_rollback_check_ == std::chrono::steady_clock::time_point{} ||
             now - last_rollback_check_ >= std::chrono::minutes(1))) {
            last_rollback_check_ = now;
            if (!rollback_check_in_flight_.exchange(true, std::memory_order_acq_rel)) {
                if (!workers_->enqueue([this]() {
                        const auto previous = runtime_->active_adapter_version();
                        const auto result = adapter_publisher_->evaluate_rollback();
                        if (!result.succeeded) {
                            std::clog << "[SRV] LoRA rollback evaluation failed: " << result.error << '\n';
                        } else if (runtime_->active_adapter_version() != previous) {
                            std::clog << "[SRV] active LoRA adapter rolled back after correction-rate regression\n";
                        }
                        rollback_check_in_flight_.store(false, std::memory_order_release);
                    })) {
                    rollback_check_in_flight_.store(false, std::memory_order_release);
                }
            }
        }
        bool training_active = false;
        {
            std::lock_guard lock(training_mutex_);
            if (training_orchestrator_) {
                const auto state = training_orchestrator_->status().state;
                training_active = state == training::TrainingOrchestratorState::Checking ||
                                  state == training::TrainingOrchestratorState::Launching ||
                                  state == training::TrainingOrchestratorState::Running;
            }
        }
        if (!training_active && sessions_->should_idle_shutdown()) request_stop();
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    close_connections();
    {
        std::lock_guard lock(connections_mutex_);
        for (auto& thread : connection_threads_) {
            if (thread.joinable()) thread.join();
        }
        connection_threads_.clear();
    }
    if (workers_) workers_->shutdown();
    if (sessions_) sessions_->shutdown();
    cleanup_endpoint();
    return 0;
}

protocol::Capabilities UnixSocketServer::supported_capabilities() const noexcept {
    auto capabilities = protocol::capability_mask(protocol::Capability::DeletePersonalData);
    if (feedback_store_ && feedback_store_->available()) capabilities |= protocol::capability_mask(protocol::Capability::TrainingStatus);
    if (feedback_store_ && feedback_store_->available() && feedback_store_->learning_enabled() &&
        valid_sha256(options_.runtime.base_model_sha256)) {
        capabilities |= protocol::capability_mask(protocol::Capability::PersonalFeedback);
    }
    return capabilities;
}

void UnixSocketServer::initialize_training() {
    if ((!options_.personal_learning_enabled && !options_.lora_training.enabled) || !feedback_store_ ||
        !feedback_store_->available()) return;
    if (options_.runtime.model_path.empty() || !valid_sha256(options_.runtime.base_model_sha256)) {
        training_configuration_error_ = "LoRA adapters require a GGUF inference model and verified base-model SHA256";
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        return;
    }
    training::AdapterPublisherOptions publisher_options;
    publisher_options.directory = options_.lora_training.adapter_directory.value_or(feedback_store_->data_directory() / "adapters");
    publisher_options.base_model_sha256 = options_.runtime.base_model_sha256;
    publisher_options.minimum_validation_characters = training::TrainingThresholds{}.minimum_validation_characters;
    try {
        publisher_options.runtime_model_sha256 = training::sha256_file(options_.runtime.model_path);
        publisher_options.tokenizer_sha256 = training::sha256_file_set(
            options_.runtime.tables_dir,
            {"tokens/chars.json", "tokens/latin.json", "tokens/special_tokens.json", "tokens/bpmf.json"});
        publisher_options.candidate_map_sha256 = training::sha256_file(options_.runtime.tables_dir / "bopomofo_char.json");
    } catch (const std::exception& error) {
        training_configuration_error_ = std::string("fingerprint serving model bundle failed: ") + error.what();
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        return;
    }
#if IMESVC_HAS_LLAMA
    publisher_options.validate_loadable = [runtime = runtime_](const std::filesystem::path& path) {
        LlamaEngine::validate_adapter(runtime, path);
    };
    publisher_options.validate_runtime = [this](const std::filesystem::path& adapter,
                                                 const std::vector<training::DatasetSample>& samples,
                                                 training::TrainingRunKind kind) {
        return validate_serving_runtime(adapter, samples, kind, runtime_, runtime_->adapter_generation(),
                                        options_.runtime.tables_dir / "bopomofo_char.json");
    };
    publisher_options.transition_activation = [this](const training::TrainingRunContext& run,
                                                       const std::function<training::StoreOperationResult()>& activate) {
        std::lock_guard activation_lock(activation_mutex_);
        training::StoreOperationResult result{false, "adapter activation did not run"};
        sessions_->with_predictions_stopped([&]() {
            if (last_prediction_activity_millis_.load(std::memory_order_acquire) > run.launch_heartbeat_millis) {
                result = {false, "inference activity resumed before adapter activation"};
                return;
            }
            result = activate();
        });
        return result;
    };
#else
    training_configuration_error_ = "LoRA adapters require a service build with llama.cpp inference support";
    std::clog << "[SRV] " << training_configuration_error_ << '\n';
    return;
#endif
    try {
        adapter_publisher_ = std::make_unique<training::AdapterPublisher>(*feedback_store_, *runtime_, publisher_options);
        if (const auto restored = adapter_publisher_->restore_active_adapter(); !restored.succeeded) {
            std::clog << "[SRV] active LoRA adapter was not restored: " << restored.error << '\n';
        }
    } catch (const std::exception& error) {
        training_configuration_error_ = std::string("initialize LoRA adapter serving failed: ") + error.what();
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        adapter_publisher_.reset();
        return;
    }
    if (!options_.lora_training.enabled) return;
    if (options_.lora_training.base_safetensors.empty() || options_.lora_training.trainer_executable.empty()) {
        training_configuration_error_ = "LoRA training requires F32 Safetensors base weights and a trainer executable";
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        return;
    }
    std::error_code base_error;
    const auto base_status = std::filesystem::symlink_status(options_.lora_training.base_safetensors, base_error);
    if (base_error || std::filesystem::is_symlink(base_status) || !std::filesystem::is_regular_file(base_status)) {
        training_configuration_error_ = "configured F32 Safetensors base weights are not a regular non-symlink file";
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        return;
    }
    try {
        if (training::sha256_file(options_.lora_training.base_safetensors) != options_.runtime.base_model_sha256) {
            training_configuration_error_ = "configured F32 Safetensors SHA256 does not match the service base-model identity";
            std::clog << "[SRV] " << training_configuration_error_ << '\n';
            return;
        }
    } catch (const std::exception& error) {
        training_configuration_error_ = std::string("verify F32 Safetensors base weights failed: ") + error.what();
        std::clog << "[SRV] " << training_configuration_error_ << '\n';
        return;
    }
    training::TrainingOrchestratorOptions orchestrator_options;
    orchestrator_options.trainer_executable = options_.lora_training.trainer_executable;
    orchestrator_options.staging_directory = feedback_store_->data_directory() / "training-staging";
    orchestrator_options.trainer_arguments = {
        "--base-safetensors", options_.lora_training.base_safetensors.string(),
        "--runtime-model", options_.runtime.model_path.string(),
        "--base-sha256", options_.runtime.base_model_sha256,
        "--tables", options_.runtime.tables_dir.string(),
    };
    auto environment = training::make_system_training_environment_provider(feedback_store_->data_directory());
    training_orchestrator_ = std::make_unique<training::TrainingOrchestrator>(
        *feedback_store_, std::move(orchestrator_options), std::move(environment),
        [this](const training::TrainingRunContext& run) {
            return adapter_publisher_->handle_completed_run(run);
        });
    // Establish the idle baseline even when no prediction has occurred in this service process yet.
    record_prediction_activity();
}

void UnixSocketServer::record_prediction_activity() noexcept {
    {
        std::lock_guard activation_lock(activation_mutex_);
        auto previous = last_prediction_activity_millis_.load(std::memory_order_relaxed);
        auto observed = training::TrainingOrchestrator::current_unix_millis();
        do {
            if (observed <= previous) observed = previous == std::numeric_limits<std::int64_t>::max() ? previous : previous + 1;
        } while (!last_prediction_activity_millis_.compare_exchange_weak(
            previous, observed, std::memory_order_release, std::memory_order_relaxed));
    }
    std::lock_guard lock(training_mutex_);
    if (training_orchestrator_) training_orchestrator_->record_prediction_activity();
}

training::StoreOperationResult UnixSocketServer::delete_personal_data() {
    std::lock_guard deletion_lock(personal_data_mutex_);
    const auto data_directory = std::filesystem::absolute(
        options_.training_data_directory.value_or(training::FeedbackStore::default_data_directory())).lexically_normal();
    struct stat data_status {};
    if (::lstat(data_directory.c_str(), &data_status) != 0) {
        if (errno != ENOENT) return {false, "inspect personal-data directory failed: " + std::string(std::strerror(errno))};
    } else if (!S_ISDIR(data_status.st_mode) || S_ISLNK(data_status.st_mode) || !owned_by_current_user(data_status)) {
        return {false, "refusing unsafe personal-data directory"};
    }
    const auto remove_managed_tree = [&data_directory](const std::filesystem::path& path,
                                                        std::string_view description) -> training::StoreOperationResult {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        const auto relative = absolute.lexically_relative(data_directory);
        if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..") {
            return {false, "refusing " + std::string(description) + " outside the personal-data directory"};
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(absolute, status_error);
        if (status_error == std::errc::no_such_file_or_directory) return {true, {}};
        if (status_error) return {false, "inspect " + std::string(description) + " failed: " + status_error.message()};
        std::error_code remove_error;
        if (std::filesystem::is_symlink(status)) {
            (void)std::filesystem::remove(absolute, remove_error);
        } else {
            std::filesystem::remove_all(absolute, remove_error);
        }
        return remove_error ? training::StoreOperationResult{false, "remove " + std::string(description) + " failed: " + remove_error.message()}
                            : training::StoreOperationResult{true, {}};
    };
    {
        std::lock_guard lock(training_mutex_);
        // Destroying the orchestrator joins its worker and terminates its child before SQLite data is removed.
        training_orchestrator_.reset();
    }
    training::StoreOperationResult artifacts{true, {}};
    sessions_->with_predictions_stopped([&]() {
        if (adapter_publisher_) artifacts = adapter_publisher_->delete_all_artifacts();
        if (artifacts.succeeded) sessions_->reset_engines();
    });
    if (!adapter_publisher_ && artifacts.succeeded) {
        const auto configured = options_.lora_training.adapter_directory.value_or(data_directory / "adapters");
        artifacts = remove_managed_tree(configured, "published LoRA adapters");
    }
    const auto staging = remove_managed_tree(data_directory / "training-staging", "training staging artifacts");
    if (feedback_store_) {
        const auto deleted = feedback_store_->delete_all_personal_data().get();
        if (!deleted.succeeded) return deleted;
    } else {
        for (const auto* filename : {"feedback.sqlite3", "feedback.sqlite3-wal", "feedback.sqlite3-shm"}) {
            std::error_code remove_error;
            const auto path = data_directory / filename;
            const auto status = std::filesystem::symlink_status(path, remove_error);
            if (remove_error == std::errc::no_such_file_or_directory) continue;
            if (remove_error) return {false, "inspect personal-data database artifact failed: " + remove_error.message()};
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
                return {false, "refusing unsafe personal-data database artifact"};
            }
            std::filesystem::remove(path, remove_error);
            if (remove_error) return {false, "remove personal-data database artifact failed: " + remove_error.message()};
        }
    }
    if (!artifacts.succeeded) return artifacts;
    if (!staging.succeeded) return staging;
    return {true, {}};
}

void UnixSocketServer::request_stop() noexcept {
    stopping_.store(true, std::memory_order_release);
}

void UnixSocketServer::accept_connections() {
    sockaddr_un address {};
    socklen_t length = sizeof(address);
    const int connection_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
    if (connection_fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (stopping_.load(std::memory_order_acquire)) return;
        throw std::system_error(errno, std::generic_category(), "accept Unix socket");
    }
    try {
        const auto uid = peer_uid(connection_fd);
        if (uid != static_cast<std::uint64_t>(::getuid())) {
            ::close(connection_fd);
            return;
        }
#ifdef FD_CLOEXEC
        (void)::fcntl(connection_fd, F_SETFD, FD_CLOEXEC);
#endif
        const timeval timeout{30, 0};
        (void)::setsockopt(connection_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(connection_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        auto connection = std::make_shared<Connection>(*this, connection_fd, uid);
        std::lock_guard lock(connections_mutex_);
        connections_.push_back(connection);
        connection_threads_.emplace_back([connection]() { connection->run(); });
    } catch (...) {
        ::close(connection_fd);
        throw;
    }
}

void UnixSocketServer::close_connections() noexcept {
    std::lock_guard lock(connections_mutex_);
    for (const auto& connection : connections_) connection->close();
}

void UnixSocketServer::reap_connections() {
    std::lock_guard lock(connections_mutex_);
    for (std::size_t index = 0; index < connections_.size();) {
        if (!connections_[index]->finished()) {
            ++index;
            continue;
        }
        if (connection_threads_[index].joinable()) connection_threads_[index].join();
        connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
        connection_threads_.erase(connection_threads_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void UnixSocketServer::cleanup_endpoint() noexcept {
    if (endpoint_owned_) {
        struct stat status {};
        if (::lstat(socket_path_.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) && owned_by_current_user(status)) {
            ::unlink(socket_path_.c_str());
        }
        endpoint_owned_ = false;
    }
    if (pid_owned_) {
        struct stat status {};
        if (::lstat(pid_path_.c_str(), &status) == 0 && S_ISREG(status.st_mode) && owned_by_current_user(status)) {
            ::unlink(pid_path_.c_str());
        }
        pid_owned_ = false;
    }
}

}  // namespace imesvc

#endif
