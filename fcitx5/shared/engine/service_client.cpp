#include "engine/service_client.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ipc/unix_socket.hpp"
#include "protocol/binary_codec.hpp"

namespace ime::fcitx5 {

namespace {

constexpr std::uint32_t kMaxFramePayloadBytes = 1024 * 1024;

#ifndef IME_FCITX5_SERVICE_NAME
#define IME_FCITX5_SERVICE_NAME "llavon-ime-service"
#endif

ByteVector recv_message(const UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    if (length > kMaxFramePayloadBytes) throw std::runtime_error("protocol frame is too large");
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

std::optional<std::filesystem::path> resolved_service_path() {
    if (const char* override = std::getenv("IME_FCITX5_SERVICE_PATH")) {
        if (override[0] != '\0') {
            std::filesystem::path path(override);
            if (std::filesystem::exists(path)) return path;
            return std::nullopt;
        }
    }

#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_service = std::filesystem::path(home) / "Library" / "fcitx5" / "bin" / IME_FCITX5_SERVICE_NAME;
        if (std::filesystem::exists(user_service)) return user_service;
    }
#endif

    for (const auto& path : {
#ifdef IME_FCITX5_INSTALLED_SERVICE_PATH
             std::filesystem::path(IME_FCITX5_INSTALLED_SERVICE_PATH),
#endif
             std::filesystem::path("/opt/homebrew/bin") / IME_FCITX5_SERVICE_NAME,
             std::filesystem::path("/usr/local/bin") / IME_FCITX5_SERVICE_NAME,
             std::filesystem::path("/usr/bin") / IME_FCITX5_SERVICE_NAME,
         }) {
        if (std::filesystem::exists(path)) return path;
    }

    if (const char* path_env = std::getenv("PATH")) {
        std::string_view paths(path_env);
        while (!paths.empty()) {
            const auto separator = paths.find(':');
            const auto entry = paths.substr(0, separator);
            if (!entry.empty()) {
                const auto candidate = std::filesystem::path(std::string(entry)) / IME_FCITX5_SERVICE_NAME;
                if (std::filesystem::exists(candidate)) return candidate;
            }
            if (separator == std::string_view::npos) break;
            paths.remove_prefix(separator + 1);
        }
    }

    return std::nullopt;
}

bool spawn_service() {
    const auto path = resolved_service_path();
    if (!path) return false;

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl(path->c_str(), path->c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    const pid_t wait_result = waitpid(pid, &status, WNOHANG);
    return wait_result == 0 || (wait_result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void wait_for_socket_shutdown(const std::filesystem::path& path) {
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        if (std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::socket) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

}  // namespace

ServiceClient::ServiceClient(std::filesystem::path socket) : socket_path_(std::move(socket)) {}

ServiceClient::~ServiceClient() {
    finish_worker();
}

PredictState ServiceClient::request_predict_async(PredictRequest request) {
    return request_predict_async(std::move(request), {});
}

PredictState ServiceClient::request_predict_async(PredictRequest request,
                                                  std::function<void(PredictState)> on_complete) {
    {
        std::lock_guard lock(mutex_);
        if (state_ == PredictState::Pending) return PredictState::Pending;
    }

    finish_worker();

    {
        std::lock_guard lock(mutex_);
        state_ = PredictState::Pending;
        latest_response_.reset();
    }

    worker_ = std::thread([this, request = std::move(request), on_complete = std::move(on_complete)]() {
        auto completed_state = PredictState::Unavailable;
        try {
            auto response = request_predict(request);
            {
                std::lock_guard lock(mutex_);
                latest_response_ = std::move(response);
                state_ = PredictState::Ready;
            }
            completed_state = PredictState::Ready;
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                latest_response_.reset();
                state_ = PredictState::Unavailable;
            }
            completed_state = PredictState::Unavailable;
        }
        if (on_complete) on_complete(completed_state);
    });

    return PredictState::Pending;
}

std::optional<PredictResponse> ServiceClient::latest_response() {
    {
        std::lock_guard lock(mutex_);
        if (state_ == PredictState::Pending) return std::nullopt;
    }

    finish_worker();
    std::lock_guard lock(mutex_);
    return latest_response_;
}

PredictState ServiceClient::state() {
    std::lock_guard lock(mutex_);
    return state_;
}

StatusResponse ServiceClient::status() {
    try {
        auto connection = UnixSocketClient{}.connect(socket_path_);
        connection.send_all(encode_message(ControlRequest{"status"}));
        return decode_status_response(recv_message(connection));
    } catch (const std::exception& error) {
        StatusResponse response;
        response.running = false;
        response.backend = "unavailable";
        response.error = error.what();
        return response;
    }
}

StatusResponse ServiceClient::stop() {
    try {
        auto connection = UnixSocketClient{}.connect(socket_path_);
        connection.send_all(encode_message(ControlRequest{"stop"}));
        return decode_status_response(recv_message(connection));
    } catch (const std::exception& error) {
        StatusResponse response;
        response.running = false;
        response.backend = "unavailable";
        response.error = error.what();
        return response;
    }
}

bool ServiceClient::start_service_if_needed() {
    const auto current = status();
    if (current.running) return true;
    if (current.backend != "unavailable") wait_for_socket_shutdown(socket_path_);
    return spawn_service();
}

PredictResponse ServiceClient::request_predict(const PredictRequest& request) {
    UnixSocketConnection connection;
    try {
        connection = UnixSocketClient{}.connect(socket_path_);
    } catch (const std::system_error&) {
        if (!start_service_if_needed()) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        connection = UnixSocketClient{}.connect(socket_path_);
    }

    connection.send_all(encode_message(request));
    return decode_predict_response(recv_message(connection));
}

void ServiceClient::finish_worker() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace ime::fcitx5
