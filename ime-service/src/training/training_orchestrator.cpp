#include "training/training_orchestrator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif

namespace imesvc::training {
namespace {

std::int64_t unix_seconds() noexcept {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool create_private_directory(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) return false;
#ifndef _WIN32
        struct stat information {};
        if (::lstat(path.c_str(), &information) != 0 || !S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) return false;
        return ::chmod(path.c_str(), S_IRWXU) == 0;
#else
        return std::filesystem::is_directory(path, error) && !error;
#endif
    } catch (...) {
        return false;
    }
}

bool create_parent_directory(const std::filesystem::path& path) noexcept {
    const auto parent = path.parent_path();
    if (parent.empty()) return true;
    try {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        return !error && std::filesystem::is_directory(parent, error) && !error;
    } catch (...) {
        return false;
    }
}

#ifndef _WIN32
bool write_all(int descriptor, const char* data, std::size_t size) noexcept {
    while (size != 0) {
        const auto written = ::write(descriptor, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool write_heartbeat(const std::filesystem::path& path, std::int64_t heartbeat) noexcept {
    try {
        static std::atomic<std::uint64_t> sequence{0};
        if (path.empty() || !create_parent_directory(path)) return false;
        const auto temporary = path.parent_path() /
            (path.filename().string() + ".tmp." + std::to_string(static_cast<unsigned long long>(::getpid())) + "." +
             std::to_string(++sequence));
        const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (descriptor < 0) return false;
        const auto contents = std::to_string(heartbeat) + "\n";
        const bool written = write_all(descriptor, contents.data(), contents.size()) && ::fsync(descriptor) == 0;
        const int close_result = ::close(descriptor);
        if (!written || close_result != 0 || ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0 ||
            ::rename(temporary.c_str(), path.c_str()) != 0 || ::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
            ::unlink(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
#endif

std::string next_run_id() {
    static std::atomic<std::uint64_t> sequence{0};
    std::uint64_t random = 0;
    try {
        random = static_cast<std::uint64_t>(std::random_device{}()) << 32U | std::random_device{}();
    } catch (...) {
        random = sequence.load(std::memory_order_relaxed);
    }
#ifndef _WIN32
    const auto process = static_cast<std::uint64_t>(::getpid());
#else
    const auto process = 0ULL;
#endif
    return "run-" + std::to_string(TrainingOrchestrator::current_unix_millis()) + "-" + std::to_string(process) + "-" +
           std::to_string(++sequence) + "-" + std::to_string(random);
}

}  // namespace

class TrainingOrchestrator::Impl final {
public:
    Impl(FeedbackStore& store, TrainingOrchestratorOptions options, std::shared_ptr<TrainingEnvironmentProvider> environment,
         TrainingCompletionHandler completion_handler)
        : store_(store), options_(std::move(options)), environment_(std::move(environment)),
          completion_handler_(std::move(completion_handler)) {
        const auto data_directory = store_.data_directory();
        if (options_.staging_directory.empty()) options_.staging_directory = data_directory / "training-staging";
        if (options_.lock_path.empty()) options_.lock_path = data_directory / "trainer.lock";
        if (options_.heartbeat_path.empty()) options_.heartbeat_path = data_directory / "inference.heartbeat";
        std::error_code error;
        options_.staging_directory = std::filesystem::absolute(options_.staging_directory, error);
        if (!error) options_.lock_path = std::filesystem::absolute(options_.lock_path, error);
        if (!error) options_.heartbeat_path = std::filesystem::absolute(options_.heartbeat_path, error);
        if (!error && !options_.trainer_executable.empty()) options_.trainer_executable = std::filesystem::absolute(options_.trainer_executable, error);
        if (error) {
            std::lock_guard lock(status_mutex_);
            status_.state = TrainingOrchestratorState::Error;
            status_.reason = "resolve training paths failed: " + error.message();
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        stop_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void record_prediction_activity() noexcept {
        auto previous = pending_heartbeat_.load(std::memory_order_relaxed);
        std::int64_t observed = TrainingOrchestrator::current_unix_millis();
        do {
            if (observed <= previous) {
                observed = previous == std::numeric_limits<std::int64_t>::max() ? previous : previous + 1;
            }
        } while (!pending_heartbeat_.compare_exchange_weak(previous, observed, std::memory_order_release,
                                                            std::memory_order_relaxed));
        wake_.notify_one();
    }

    void request_evaluation() noexcept {
        evaluation_requested_.store(true, std::memory_order_release);
        wake_.notify_one();
    }

    TrainingOrchestratorStatus status() const {
        std::lock_guard lock(status_mutex_);
        return status_;
    }

private:
    void run() noexcept {
#ifndef _WIN32
        recover_incomplete_runs();
#endif
        while (!stop_.load(std::memory_order_acquire)) {
            flush_heartbeat();
#ifndef _WIN32
            poll_child();
#endif
            const auto requested = evaluation_requested_.exchange(false, std::memory_order_acq_rel);
            const auto now = TrainingOrchestrator::current_unix_millis();
            if (requested || now - last_evaluation_millis_ >= evaluation_interval_millis()) {
                last_evaluation_millis_ = now;
#ifndef _WIN32
                if (child_process_id_ == 0) evaluate();
#else
                evaluate();
#endif
            }
            std::unique_lock lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(std::max<std::int64_t>(1, evaluation_interval_millis())), [this] {
                return stop_.load(std::memory_order_acquire) || evaluation_requested_.load(std::memory_order_acquire) ||
                       pending_heartbeat_.load(std::memory_order_acquire) != written_heartbeat_;
            });
        }
#ifndef _WIN32
        stop_child();
#endif
        set_status(TrainingOrchestratorState::Stopped, "training orchestration stopped");
    }

    std::int64_t evaluation_interval_millis() const noexcept {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(options_.evaluation_interval).count();
        return milliseconds > 0 ? milliseconds : 1;
    }

    void flush_heartbeat() noexcept {
        const auto heartbeat = pending_heartbeat_.load(std::memory_order_acquire);
        if (heartbeat == 0 || heartbeat == written_heartbeat_) return;
#ifndef _WIN32
        if (write_heartbeat(options_.heartbeat_path, heartbeat)) written_heartbeat_ = heartbeat;
#else
        written_heartbeat_ = heartbeat;
#endif
    }

    void set_status(TrainingOrchestratorState state, std::string reason, std::string run_id = {}, std::int64_t process_id = 0,
                    const TrainingAccounting* accounting = nullptr) {
        std::lock_guard lock(status_mutex_);
        status_.state = state;
        status_.reason = std::move(reason);
        if (!run_id.empty() || state != TrainingOrchestratorState::Running) status_.run_id = std::move(run_id);
        status_.trainer_process_id = process_id;
        if (accounting != nullptr) status_.accounting = *accounting;
    }

    void evaluate() noexcept {
        try {
            set_status(TrainingOrchestratorState::Checking, "checking training conditions");
            const auto accounting = store_.training_accounting().get();
            set_status(TrainingOrchestratorState::Checking, "checking training conditions", {}, 0, &accounting);
            std::optional<TrainingRunKind> kind;
#ifndef _WIN32
            if (paused_run_id_.empty())
#endif
            {
                const auto backoff = std::chrono::duration_cast<std::chrono::seconds>(options_.failure_retry_backoff).count();
                const auto now_seconds = unix_seconds();
                if (accounting.last_training_failed && backoff > 0 && accounting.last_training_completed_at_unix_seconds > 0 &&
                    now_seconds >= accounting.last_training_completed_at_unix_seconds &&
                    now_seconds - accounting.last_training_completed_at_unix_seconds < backoff) {
                    set_status(TrainingOrchestratorState::Blocked, "training retry backoff is active", {}, 0, &accounting);
                    return;
                }
                kind = training_kind(accounting);
                if (!kind.has_value()) {
                    set_status(TrainingOrchestratorState::Blocked, "training data threshold is not met", {}, 0, &accounting);
                    return;
                }
            }
            if (const auto idle_reason = idle_block_reason(); idle_reason.has_value()) {
                set_status(TrainingOrchestratorState::Blocked, *idle_reason, {}, 0, &accounting);
                return;
            }
            if (const auto environment_reason = environment_block_reason(); environment_reason.has_value()) {
                set_status(TrainingOrchestratorState::Blocked, *environment_reason, {}, 0, &accounting);
                return;
            }
#ifdef _WIN32
            set_status(TrainingOrchestratorState::Error, "training launch is only available on POSIX", {}, 0, &accounting);
            return;
#else
            if (options_.trainer_executable.empty() || !std::filesystem::is_regular_file(options_.trainer_executable)) {
                set_status(TrainingOrchestratorState::Blocked, "trainer executable is unavailable", {}, 0, &accounting);
                return;
            }
            if (!create_private_directory(options_.staging_directory)) {
                set_status(TrainingOrchestratorState::Error, "create private training staging directory failed", {}, 0, &accounting);
                return;
            }
            if (!acquire_lock()) {
                set_status(TrainingOrchestratorState::Blocked, "another trainer owns the process lock", {}, 0, &accounting);
                return;
            }
            if (!paused_run_id_.empty()) {
                active_kind_ = paused_kind_;
                active_staging_directory_ = paused_staging_directory_;
                const auto process_id = launch(paused_snapshot_id_, paused_staging_directory_);
                if (process_id <= 0) {
                    release_lock();
                    active_staging_directory_.clear();
                    set_status(TrainingOrchestratorState::Error, "resume trainer process failed", paused_run_id_, 0, &accounting);
                    return;
                }
                child_process_id_ = process_id;
                active_run_id_ = std::move(paused_run_id_);
                active_snapshot_id_ = std::move(paused_snapshot_id_);
                active_staging_directory_ = std::move(paused_staging_directory_);
                set_status(TrainingOrchestratorState::Running, "resumed trainer process is running",
                           active_run_id_, child_process_id_, &accounting);
                return;
            }
            set_status(TrainingOrchestratorState::Launching, "creating immutable training snapshot", {}, 0, &accounting);
            const auto snapshot = store_.create_dataset_snapshot(options_.snapshot_options).get();
            if (!snapshot.operation.succeeded) {
                release_lock();
                set_status(TrainingOrchestratorState::Error, "create training snapshot failed: " + snapshot.operation.error, {}, 0, &accounting);
                return;
            }
            if (const auto idle_reason = idle_block_reason(); idle_reason.has_value()) {
                release_lock();
                set_status(TrainingOrchestratorState::Blocked, *idle_reason, {}, 0, &accounting);
                return;
            }
            TrainingRunStart run;
            run.run_id = next_run_id();
            run.snapshot_id = snapshot.snapshot.snapshot_id;
            run.kind = *kind;
            run.started_at_unix_seconds = unix_seconds();
            run.eligible_target_characters = accounting.eligible_target_characters;
            run.eligible_samples = accounting.eligible_samples;
            const auto recorded = store_.record_training_started(run).get();
            if (!recorded.succeeded) {
                release_lock();
                set_status(TrainingOrchestratorState::Error, "record training start failed: " + recorded.error, {}, 0, &accounting);
                return;
            }
            const auto staging_directory = options_.staging_directory / run.run_id;
            if (!create_private_directory(staging_directory)) {
                (void)store_.record_training_finished(run.run_id, false).get();
                release_lock();
                set_status(TrainingOrchestratorState::Error, "create per-run training staging directory failed", run.run_id, 0, &accounting);
                return;
            }
            active_kind_ = run.kind;
            active_staging_directory_ = staging_directory;
            const auto process_id = launch(snapshot.snapshot.snapshot_id, staging_directory);
            if (process_id <= 0) {
                (void)store_.record_training_finished(run.run_id, false).get();
                release_lock();
                active_staging_directory_.clear();
                set_status(TrainingOrchestratorState::Error, "launch trainer process failed", run.run_id, 0, &accounting);
                return;
            }
            child_process_id_ = process_id;
            active_run_id_ = std::move(run.run_id);
            active_snapshot_id_ = snapshot.snapshot.snapshot_id;
            active_kind_ = run.kind;
            active_staging_directory_ = staging_directory;
            set_status(TrainingOrchestratorState::Running, "trainer process is running", active_run_id_, child_process_id_, &accounting);
#endif
        } catch (const std::exception& error) {
            set_status(TrainingOrchestratorState::Error, std::string("training evaluation failed: ") + error.what());
        } catch (...) {
            set_status(TrainingOrchestratorState::Error, "training evaluation failed");
        }
    }

    std::optional<TrainingRunKind> training_kind(const TrainingAccounting& accounting) const noexcept {
        const auto& thresholds = options_.thresholds;
        if (!accounting.shadow_completed) {
            if (accounting.eligible_target_characters >= thresholds.shadow_smoke_characters) return TrainingRunKind::ShadowSmoke;
            return std::nullopt;
        }
        if (!accounting.has_active_adapter) {
            if (accounting.eligible_target_characters >= thresholds.first_adapter_characters &&
                accounting.eligible_samples >= thresholds.first_adapter_commits &&
                accounting.validation_target_characters >= thresholds.minimum_validation_characters) {
                return TrainingRunKind::FirstAdapter;
            }
            return std::nullopt;
        }
        const auto new_characters = accounting.eligible_target_characters >= accounting.eligible_target_characters_at_last_success
                                        ? accounting.eligible_target_characters - accounting.eligible_target_characters_at_last_success
                                        : 0;
        if (new_characters < thresholds.later_training_new_characters ||
            accounting.validation_target_characters < thresholds.minimum_validation_characters) {
            return std::nullopt;
        }
        const auto now = unix_seconds();
        const auto minimum_interval = std::chrono::duration_cast<std::chrono::seconds>(thresholds.minimum_interval).count();
        if (accounting.last_training_completed_at_unix_seconds > 0 &&
            (now < accounting.last_training_completed_at_unix_seconds ||
             now - accounting.last_training_completed_at_unix_seconds < minimum_interval)) {
            return std::nullopt;
        }
        return TrainingRunKind::Incremental;
    }

    std::optional<std::string> idle_block_reason() const noexcept {
        const auto heartbeat = TrainingOrchestrator::read_inference_heartbeat(options_.heartbeat_path);
        if (!heartbeat.has_value()) return "inference heartbeat is unavailable";
        const auto now = TrainingOrchestrator::current_unix_millis();
        const auto required_idle = std::chrono::duration_cast<std::chrono::milliseconds>(options_.required_idle).count();
        if (*heartbeat > now || now - *heartbeat < required_idle) return "inference has not been idle long enough";
        return std::nullopt;
    }

    std::optional<std::string> environment_block_reason() noexcept {
        if (!environment_) return "training environment is unavailable";
        try {
            const auto environment = environment_->current_environment();
            if (!environment.has_value()) return "training environment is unavailable";
            if (!environment->on_ac_power) return "system is not on AC power";
            if (environment->battery_percentage <= 50) return "battery level is not above 50 percent";
            if (!environment->thermal_acceptable) return "thermal state is not acceptable";
            if (!std::isfinite(environment->cpu_load_fraction) || environment->cpu_load_fraction < 0.0 ||
                environment->cpu_load_fraction > options_.maximum_cpu_load_fraction) {
                return "system CPU load is too high";
            }
            if (environment->free_memory_bytes < options_.minimum_free_memory_bytes) return "insufficient free memory";
            if (environment->free_disk_bytes < options_.minimum_free_disk_bytes) return "insufficient free disk space";
            return std::nullopt;
        } catch (...) {
            return "training environment check failed";
        }
    }

#ifndef _WIN32
    void recover_incomplete_runs() noexcept {
        try {
            const auto incomplete = store_.incomplete_training_runs().get();
            if (!incomplete.operation.succeeded) {
                set_status(TrainingOrchestratorState::Error,
                           "recover incomplete training runs failed: " + incomplete.operation.error);
                return;
            }
            for (std::size_t index = 1; index < incomplete.runs.size(); ++index) {
                (void)store_.record_training_finished(incomplete.runs[index].run_id, false).get();
                remove_staging(options_.staging_directory / incomplete.runs[index].run_id);
            }
            if (incomplete.runs.empty()) return;
            const auto& run = incomplete.runs.front();
            const auto staging = options_.staging_directory / run.run_id;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(staging, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
                (void)store_.record_training_finished(run.run_id, false).get();
                remove_staging(staging);
                return;
            }
            paused_run_id_ = run.run_id;
            paused_snapshot_id_ = run.snapshot_id;
            paused_kind_ = run.kind;
            paused_staging_directory_ = staging;
            set_status(TrainingOrchestratorState::Waiting, "recovered interrupted training run", run.run_id);
        } catch (const std::exception& error) {
            set_status(TrainingOrchestratorState::Error,
                       std::string("recover incomplete training runs failed: ") + error.what());
        } catch (...) {
            set_status(TrainingOrchestratorState::Error, "recover incomplete training runs failed");
        }
    }

    void remove_staging(const std::filesystem::path& path) noexcept {
        if (path.empty()) return;
        try {
            const auto root = std::filesystem::absolute(options_.staging_directory);
            const auto candidate = std::filesystem::absolute(path);
            const auto relative = candidate.lexically_relative(root);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || relative == ".") return;
            std::error_code error;
            std::filesystem::remove_all(candidate, error);
        } catch (...) {
        }
    }
#endif

#ifndef _WIN32
    bool acquire_lock() noexcept {
        if (lock_descriptor_ >= 0) return true;
        if (!create_parent_directory(options_.lock_path)) return false;
        int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const int descriptor = ::open(options_.lock_path.c_str(), flags, S_IRUSR | S_IWUSR);
        if (descriptor < 0) return false;
        if (::chmod(options_.lock_path.c_str(), S_IRUSR | S_IWUSR) != 0 || ::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor);
            return false;
        }
        lock_descriptor_ = descriptor;
        return true;
    }

    void release_lock() noexcept {
        if (lock_descriptor_ < 0) return;
        (void)::flock(lock_descriptor_, LOCK_UN);
        (void)::close(lock_descriptor_);
        lock_descriptor_ = -1;
    }

    std::int64_t launch(const std::string& snapshot_id, const std::filesystem::path& staging_directory) noexcept {
        std::vector<std::string> arguments;
        try {
            const auto launch_heartbeat = TrainingOrchestrator::read_inference_heartbeat(options_.heartbeat_path);
            if (!launch_heartbeat) return 0;
            active_launch_heartbeat_ = *launch_heartbeat;
            arguments.reserve(options_.trainer_arguments.size() + 17U);
            arguments.push_back(options_.trainer_executable.string());
            arguments.insert(arguments.end(), options_.trainer_arguments.begin(), options_.trainer_arguments.end());
            arguments.push_back("--dataset-snapshot-id");
            arguments.push_back(snapshot_id);
            arguments.push_back("--feedback-database");
            arguments.push_back(store_.database_path().string());
            arguments.push_back("--staging-directory");
            arguments.push_back(staging_directory.string());
            arguments.push_back("--training-run-kind");
            switch (active_kind_) {
                case TrainingRunKind::ShadowSmoke: arguments.push_back("shadow-smoke"); break;
                case TrainingRunKind::FirstAdapter: arguments.push_back("first-adapter"); break;
                case TrainingRunKind::Incremental: arguments.push_back("incremental"); break;
            }
            arguments.push_back("--inference-heartbeat");
            arguments.push_back(options_.heartbeat_path.string());
            arguments.push_back("--launch-heartbeat-millis");
            arguments.push_back(std::to_string(*launch_heartbeat));
            arguments.push_back("--intraop-threads");
            arguments.push_back(std::to_string(options_.trainer_intraop_threads));
            arguments.push_back("--interop-threads");
            arguments.push_back(std::to_string(options_.trainer_interop_threads));
        } catch (...) {
            return 0;
        }
        std::vector<char*> argv;
        try {
            argv.reserve(arguments.size() + 1U);
            for (auto& argument : arguments) argv.push_back(argument.data());
            argv.push_back(nullptr);
        } catch (...) {
            return 0;
        }
        const auto process_id = ::fork();
        if (process_id < 0) return 0;
        if (process_id == 0) {
            (void)::setpgid(0, 0);
#ifdef __linux__
            (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (::getppid() == 1) _exit(128 + SIGTERM);
#endif
            if (lock_descriptor_ >= 0) (void)::close(lock_descriptor_);
            (void)::setpriority(PRIO_PROCESS, 0, 15);
#ifdef __linux__
#ifdef SYS_ioprio_set
            // IOPRIO_CLASS_IDLE (3) and level 7 are best effort; unsupported kernels simply reject it.
            (void)::syscall(SYS_ioprio_set, 1, 0, (3 << 13) | 7);
#endif
#endif
            if (::chdir(staging_directory.c_str()) != 0) _exit(126);
            ::execv(argv.front(), argv.data());
            _exit(127);
        }
        (void)::setpgid(static_cast<pid_t>(process_id), static_cast<pid_t>(process_id));
        return process_id;
    }

    void poll_child() noexcept {
        if (child_process_id_ <= 0) return;
        int process_status = 0;
        const auto waited = ::waitpid(static_cast<pid_t>(child_process_id_), &process_status, WNOHANG);
        if (waited == 0) return;
        const bool paused = waited == child_process_id_ && WIFEXITED(process_status) && WEXITSTATUS(process_status) == 2;
        if (paused) {
            paused_run_id_ = active_run_id_;
            paused_snapshot_id_ = active_snapshot_id_;
            paused_kind_ = active_kind_;
            paused_staging_directory_ = active_staging_directory_;
            release_lock();
            const auto accounting = store_.training_accounting().get();
            set_status(TrainingOrchestratorState::Waiting, "trainer paused after inference activity",
                       paused_run_id_, 0, &accounting);
            child_process_id_ = 0;
            active_run_id_.clear();
            active_snapshot_id_.clear();
            active_staging_directory_.clear();
            return;
        }
        const bool succeeded = waited == child_process_id_ && WIFEXITED(process_status) && WEXITSTATUS(process_status) == 0;
        bool completed = succeeded;
        std::string completion_reason;
        const auto completion_heartbeat = TrainingOrchestrator::read_inference_heartbeat(options_.heartbeat_path);
        if (completed && (!completion_heartbeat || *completion_heartbeat > active_launch_heartbeat_)) {
            completed = false;
            completion_reason = "inference activity resumed before adapter activation";
        }
        if (completed && completion_handler_ && !active_run_id_.empty()) {
            try {
                set_status(TrainingOrchestratorState::Checking, "validating and publishing trained adapter", active_run_id_);
                const auto result = completion_handler_(TrainingRunContext{
                    active_run_id_, active_snapshot_id_, active_kind_, active_staging_directory_, active_launch_heartbeat_});
                if (!result.succeeded) {
                    completed = false;
                    completion_reason = result.error;
                }
            } catch (const std::exception& error) {
                completed = false;
                completion_reason = error.what();
            } catch (...) {
                completed = false;
                completion_reason = "adapter validation failed";
            }
        }
        if (!completed) remove_staging(active_staging_directory_);
        const auto finished = active_run_id_.empty()
                                  ? StoreOperationResult{true, {}}
                                  : store_.record_training_finished(active_run_id_, completed).get();
        if (!finished.succeeded) {
            completed = false;
            completion_reason = finished.error;
        }
        release_lock();
        const auto accounting = store_.training_accounting().get();
        set_status(completed ? TrainingOrchestratorState::Waiting : TrainingOrchestratorState::Error,
                   completed ? "trainer process completed" : (completion_reason.empty() ? "trainer process failed" : completion_reason),
                   active_run_id_, 0, &accounting);
        child_process_id_ = 0;
        active_run_id_.clear();
        active_snapshot_id_.clear();
        active_staging_directory_.clear();
        active_launch_heartbeat_ = 0;
    }

    void stop_child() noexcept {
        if (child_process_id_ <= 0) {
            if (!paused_run_id_.empty()) {
                (void)store_.record_training_finished(paused_run_id_, false).get();
                remove_staging(paused_staging_directory_);
                paused_run_id_.clear();
                paused_snapshot_id_.clear();
                paused_staging_directory_.clear();
            }
            release_lock();
            return;
        }
        (void)::kill(-static_cast<pid_t>(child_process_id_), SIGTERM);
        (void)::kill(static_cast<pid_t>(child_process_id_), SIGTERM);
        int process_status = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::waitpid(static_cast<pid_t>(child_process_id_), &process_status, WNOHANG) == child_process_id_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (::waitpid(static_cast<pid_t>(child_process_id_), &process_status, WNOHANG) == 0) {
            (void)::kill(-static_cast<pid_t>(child_process_id_), SIGKILL);
            (void)::kill(static_cast<pid_t>(child_process_id_), SIGKILL);
            (void)::waitpid(static_cast<pid_t>(child_process_id_), &process_status, 0);
        }
        if (!active_run_id_.empty()) (void)store_.record_training_finished(active_run_id_, false).get();
        remove_staging(active_staging_directory_);
        child_process_id_ = 0;
        active_run_id_.clear();
        active_snapshot_id_.clear();
        active_staging_directory_.clear();
        active_launch_heartbeat_ = 0;
        release_lock();
    }
#endif

    FeedbackStore& store_;
    TrainingOrchestratorOptions options_;
    std::shared_ptr<TrainingEnvironmentProvider> environment_;
    TrainingCompletionHandler completion_handler_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> evaluation_requested_{false};
    std::atomic<std::int64_t> pending_heartbeat_{0};
    std::int64_t written_heartbeat_ = 0;
    std::int64_t last_evaluation_millis_ = 0;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    mutable std::mutex status_mutex_;
    TrainingOrchestratorStatus status_;
#ifndef _WIN32
    int lock_descriptor_ = -1;
    std::int64_t child_process_id_ = 0;
    std::string active_run_id_;
    std::string active_snapshot_id_;
    TrainingRunKind active_kind_ = TrainingRunKind::ShadowSmoke;
    std::filesystem::path active_staging_directory_;
    std::int64_t active_launch_heartbeat_ = 0;
    std::string paused_run_id_;
    std::string paused_snapshot_id_;
    TrainingRunKind paused_kind_ = TrainingRunKind::ShadowSmoke;
    std::filesystem::path paused_staging_directory_;
#endif
};

TrainingOrchestrator::TrainingOrchestrator(FeedbackStore& store, TrainingOrchestratorOptions options,
                                            std::shared_ptr<TrainingEnvironmentProvider> environment,
                                            TrainingCompletionHandler completion_handler)
    : impl_(std::make_unique<Impl>(store, std::move(options), std::move(environment), std::move(completion_handler))) {}

TrainingOrchestrator::~TrainingOrchestrator() = default;

void TrainingOrchestrator::record_prediction_activity() noexcept { impl_->record_prediction_activity(); }

void TrainingOrchestrator::request_evaluation() noexcept { impl_->request_evaluation(); }

TrainingOrchestratorStatus TrainingOrchestrator::status() const { return impl_->status(); }

std::int64_t TrainingOrchestrator::current_unix_millis() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::optional<std::int64_t> TrainingOrchestrator::read_inference_heartbeat(const std::filesystem::path& path) noexcept {
#ifndef _WIN32
    if (path.empty()) return std::nullopt;
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode) || S_ISLNK(information.st_mode) ||
        information.st_size <= 0 || information.st_size > 64) {
        return std::nullopt;
    }
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) return std::nullopt;
    std::array<char, 65> buffer{};
    ssize_t total = 0;
    while (total < information.st_size) {
        const auto read_count = ::read(descriptor, buffer.data() + total, static_cast<std::size_t>(information.st_size - total));
        if (read_count > 0) {
            total += read_count;
            continue;
        }
        if (read_count < 0 && errno == EINTR) continue;
        (void)::close(descriptor);
        return std::nullopt;
    }
    (void)::close(descriptor);
    std::string_view value(buffer.data(), static_cast<std::size_t>(total));
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (value.empty()) return std::nullopt;
    std::int64_t heartbeat = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), heartbeat);
    if (error != std::errc{} || pointer != value.data() + value.size() || heartbeat < 0) return std::nullopt;
    return heartbeat;
#else
    (void)path;
    return std::nullopt;
#endif
}

bool TrainingOrchestrator::inference_activity_since(const std::filesystem::path& path, std::int64_t previous_heartbeat) noexcept {
    const auto current = read_inference_heartbeat(path);
    return !current.has_value() || *current > previous_heartbeat;
}

}  // namespace imesvc::training
