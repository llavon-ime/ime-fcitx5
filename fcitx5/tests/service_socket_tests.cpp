#include "engine/service_client.hpp"
#include "protocol/protocol.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {

class ScopedProcess {
public:
    explicit ScopedProcess(pid_t pid) : pid_(pid) {}
    ~ScopedProcess() {
        if (pid_ <= 0) return;

        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGTERM);
            (void)waitpid(pid_, &status, 0);
        }
    }

    ScopedProcess(const ScopedProcess&) = delete;
    ScopedProcess& operator=(const ScopedProcess&) = delete;

    int wait() {
        if (pid_ <= 0) return EXIT_FAILURE;
        int status = 0;
        const pid_t result = waitpid(pid_, &status, 0);
        pid_ = -1;
        if (result < 0 || !WIFEXITED(status)) return EXIT_FAILURE;
        return WEXITSTATUS(status);
    }

private:
    pid_t pid_ = -1;
};

bool wait_for_socket(const std::filesystem::path& path) {
    for (int i = 0; i < 100; ++i) {
        std::error_code ec;
        if (std::filesystem::symlink_status(path, ec).type() == std::filesystem::file_type::socket) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

int run_service_socket_tests() {
    bool ok = true;

    const auto unique_suffix = std::to_string(getpid()) + "-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto runtime_root = std::filesystem::path("/tmp") / ("ime-fx-r-" + unique_suffix);
    const auto config_root = std::filesystem::path("/tmp") / ("ime-fx-c-" + unique_suffix);
    const auto socket_path = runtime_root / "llavon-ime" / "ime.sock";
    std::filesystem::remove_all(runtime_root);
    std::filesystem::remove_all(config_root);
    std::filesystem::create_directories(runtime_root);
    std::filesystem::create_directories(config_root);

    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", runtime_root.c_str(), 1);
        setenv("XDG_CONFIG_HOME", config_root.c_str(), 1);
        execl(IME_FCITX5_SERVICE_TEST_PATH, IME_FCITX5_SERVICE_TEST_PATH, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid < 0) return EXIT_FAILURE;
    ScopedProcess service(pid);

    ok = ok && wait_for_socket(socket_path);
    if (ok) {
        ime::fcitx5::ServiceClient client(socket_path);
        const auto status = client.status();
        ok = ok && status.running;

        ime::fcitx5::PredictRequest request;
        request.padding.push_back({false, u"ㄋㄧˇ", 0});
        ok = ok && client.request_predict_async(request) == ime::fcitx5::PredictState::Pending;

        bool received_prediction = false;
        for (int i = 0; i < 100; ++i) {
            if (auto response = client.latest_response()) {
                received_prediction = response->candidates.size() == 1 && !response->candidates.front().empty() &&
                                      response->candidates.front().front() == U'你';
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ok = ok && received_prediction;

        const auto stopped = client.stop();
        ok = ok && !stopped.running;
    }

    ok = ok && service.wait() == EXIT_SUCCESS;
    std::filesystem::remove_all(runtime_root);
    std::filesystem::remove_all(config_root);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
