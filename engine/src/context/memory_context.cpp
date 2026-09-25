#include "context/memory_context.hpp"

#include "text/utf.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace llavon::ime {

namespace {

constexpr std::size_t kTokenCodeUnits = 16;
constexpr char32_t kTokenFirst = 0xE000;
constexpr char32_t kTokenLast = 0xF8FF;
constexpr auto kProbeInterval = std::chrono::milliseconds(400);
constexpr auto kBackoffPause = std::chrono::seconds(10);
constexpr std::size_t kMaxConsecutiveFailures = 3;
constexpr auto kHelperTimeout = std::chrono::seconds(5);
constexpr std::size_t kMaxHelperOutput = 64 * 1024;

#if defined(__linux__)

std::string to_hex(std::uintptr_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lx", static_cast<unsigned long>(value));
    return buffer;
}

// Runs the helper with a stdout pipe. Returns its output, or nullopt when the
// helper could not be started. The helper enforces its own scan budget; this
// only guards against a stuck process.
std::optional<std::string> run_helper(const std::filesystem::path& helper,
                                      const std::string& token_utf8,
                                      const std::vector<int>& processes,
                                      int hint_pid, std::uintptr_t hint_address,
                                      const std::string& expect_suffix) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return std::nullopt;
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::nullopt;
    }
    if (child == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) ::_exit(127);
        ::close(pipe_fds[1]);
        std::vector<std::string> arguments;
        arguments.push_back(helper.string());
        arguments.push_back("--needle");
        arguments.push_back(token_utf8);
        arguments.push_back("--timeout-ms");
        arguments.push_back(processes.empty() ? "8000" : "4000");
        if (!expect_suffix.empty()) {
            arguments.push_back("--expect-suffix");
            arguments.push_back(expect_suffix);
        }
        if (hint_pid > 0 && hint_address > 0) {
            // A previous probe found the token here; try that window first so
            // repeat probes stay fast.
            const auto start = hint_address > 32768 ? hint_address - 32768 : 0;
            arguments.push_back("--hint");
            arguments.push_back(std::to_string(hint_pid) + ":0x" + to_hex(start) + "-0x" +
                                to_hex(hint_address + 32768));
        }
        if (processes.empty()) {
            // The client did not name its processes (XIM and friends): scan
            // every process of the user instead.
            arguments.push_back("--same-uid-all");
        } else {
            for (const int pid : processes) {
                arguments.push_back("--pid");
                arguments.push_back(std::to_string(pid));
            }
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(pipe_fds[1]);

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + kHelperTimeout;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            ::kill(child, SIGKILL);
            break;
        }
        pollfd descriptor{pipe_fds[0], POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(std::min<long long>(remaining, 1000)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        char buffer[4096];
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count <= 0) break;
        output.append(buffer, static_cast<std::size_t>(count));
        if (output.size() > kMaxHelperOutput) break;
    }
    ::close(pipe_fds[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    return output;
}

#endif

}  // namespace

class MemoryContextProvider::Impl {
public:
    struct Hooks {
        std::function<void(std::u16string, bool)> publish;
        std::function<void(AccessibilityAvailability, std::string)> availability;
        std::function<bool()> active;
        std::function<size_t()> max_code_units;
    };

    Impl(Hooks hooks, MemoryProbeCallbacks callbacks, std::filesystem::path helper_path)
        : hooks_(std::move(hooks)),
          callbacks_(std::move(callbacks)),
          helper_path_(std::move(helper_path)),
          rng_(std::random_device{}()) {}

    ~Impl() { stop(); }

    bool start() {
#if defined(__linux__)
        std::error_code error;
        if (helper_path_.empty() || !std::filesystem::exists(helper_path_, error)) {
            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-missing");
            return false;
        }
        if (!std::filesystem::is_regular_file(helper_path_, error)) {
            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-missing");
            return false;
        }
        const auto permissions = std::filesystem::status(helper_path_, error).permissions();
        if ((permissions & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-not-executable");
            return false;
        }
        if (!worker_.joinable()) worker_ = std::thread([this] { worker_loop(); });
        running_ = true;
        hooks_.availability(AccessibilityAvailability::Available, "memscan");
        return true;
#else
        hooks_.availability(AccessibilityAvailability::Unsupported, "no-backend");
        return false;
#endif
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        running_ = false;
    }

    bool running() const noexcept { return running_; }

    void refresh() {
        if (!running_) return;
        if (!hooks_.active()) return;
        if (callbacks_.sensitive && callbacks_.sensitive()) return;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_probe_ < kProbeInterval) return;
        if (failures_ >= kMaxConsecutiveFailures && now - last_probe_ < kBackoffPause) return;
        last_probe_ = now;
        if (failures_ >= kMaxConsecutiveFailures) failures_ = 0;

        const std::u16string token = make_token();
        if (!callbacks_.inject || !callbacks_.inject(token)) {
            ++failures_;
            hooks_.publish({}, false);
            return;
        }
        std::vector<int> processes = callbacks_.processes ? callbacks_.processes() : std::vector<int>{};
        Job job;
        try {
            job.token_utf8 = u16_to_utf8(token);
        } catch (const std::exception&) {
            callbacks_.remove(token.size());
            hooks_.publish({}, false);
            return;
        }
        job.token_units = token.size();
        job.processes = std::move(processes);
        if (callbacks_.expect_suffix) job.expect_suffix = callbacks_.expect_suffix();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                callbacks_.remove(job.token_units);
                return;
            }
            jobs_.push_back(std::move(job));
        }
        condition_.notify_one();
    }

    std::size_t probe_count() const noexcept { return probes_.load(); }

private:
    struct Job {
        std::string token_utf8;
        std::size_t token_units = 0;
        std::vector<int> processes;
        std::string expect_suffix;
    };

    std::u16string make_token() {
        std::uniform_int_distribution<char32_t> distribution(kTokenFirst, kTokenLast);
        std::u16string token;
        token.reserve(kTokenCodeUnits);
        for (std::size_t index = 0; index < kTokenCodeUnits; ++index) {
            token.push_back(static_cast<char16_t>(distribution(rng_)));
        }
        return token;
    }

    void worker_loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            run_job(std::move(job));
        }
    }

    void run_job(Job job) {
        ++probes_;
        std::string output;
#if defined(__linux__)
        const auto result = run_helper(helper_path_, job.token_utf8, job.processes, hint_pid_,
                                       hint_address_, job.expect_suffix);
        if (result) output = *result;
#endif
        const auto parsed = nlohmann::json::parse(output, nullptr, false);
        const bool found = !parsed.is_discarded() && parsed.value("found", false);
        if (found) {
            const std::string before = parsed.value("before", std::string{});
            try {
                hooks_.publish(utf16_tail(utf8_to_u16(before), hooks_.max_code_units()), true);
            } catch (const std::exception&) {
                hooks_.publish({}, false);
            }
            failures_ = 0;
            hint_pid_ = parsed.value("pid", 0);
            const std::string address = parsed.value("address", std::string{});
            hint_address_ = 0;
            if (!address.empty()) {
                hint_address_ = static_cast<std::uintptr_t>(std::strtoull(address.c_str(), nullptr, 16));
            }
            hooks_.availability(AccessibilityAvailability::Available, "memscan");
        } else {
            ++failures_;
            hooks_.publish({}, false);
            const std::string code =
                parsed.is_discarded() ? "helper-failed" : parsed.value("error", "helper-failed");
            if (code == "denied") {
                hooks_.availability(AccessibilityAvailability::Unavailable, "permission-denied");
            } else if (code == "helper-missing") {
                hooks_.availability(AccessibilityAvailability::Unavailable, "helper-missing");
            }
        }
        // The probe token must leave the document again even when the scan
        // failed; the engine marshals this onto the main thread.
        if (callbacks_.remove) callbacks_.remove(job.token_units);
    }

    Hooks hooks_;
    MemoryProbeCallbacks callbacks_;
    std::filesystem::path helper_path_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> probes_{0};
    bool stopping_ = false;
    std::size_t failures_ = 0;
    std::chrono::steady_clock::time_point last_probe_{};
    // Where the token was found last time, so the next probe can look there
    // first instead of scanning everything.
    int hint_pid_ = 0;
    std::uintptr_t hint_address_ = 0;
    std::mt19937_64 rng_;
};

MemoryContextProvider::MemoryContextProvider(size_t max_code_units, MemoryProbeCallbacks callbacks,
                                             std::filesystem::path helper_path)
    : AccessibilityContextProvider(max_code_units) {
    Impl::Hooks hooks;
    hooks.publish = [this](std::u16string text, bool usable) { publish(std::move(text), usable); };
    hooks.availability = [this](AccessibilityAvailability availability, std::string detail) {
        set_availability(availability, std::move(detail));
    };
    hooks.active = [this] { return active(); };
    hooks.max_code_units = [this] { return AccessibilityContextProvider::max_code_units(); };
    impl_ = std::make_unique<Impl>(std::move(hooks), std::move(callbacks), std::move(helper_path));
}

MemoryContextProvider::~MemoryContextProvider() = default;

bool MemoryContextProvider::start() { return impl_->start(); }

void MemoryContextProvider::stop() { impl_->stop(); }

bool MemoryContextProvider::running() const noexcept { return impl_->running(); }

void MemoryContextProvider::refresh() { impl_->refresh(); }

std::size_t MemoryContextProvider::probe_count() const noexcept { return impl_->probe_count(); }

bool memory_context_supported() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

}  // namespace llavon::ime
