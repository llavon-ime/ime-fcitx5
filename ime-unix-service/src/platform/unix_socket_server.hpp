#pragma once

#include "../engine/core_runtime.hpp"
#include "../session/session_manager.hpp"
#include "server_strategy.hpp"

#include <filesystem>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ime::unix_service {

struct UnixServerOptions {
    RuntimeConfig runtime;
    SessionLimits limits;
    std::optional<std::filesystem::path> socket_path;
    std::optional<std::filesystem::path> pid_path;
};

class UnixSocketServer final : public ServerStrategy {
public:
    explicit UnixSocketServer(UnixServerOptions options);
    ~UnixSocketServer() override;

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    const char* name() const override;
    int run() override;

    static std::filesystem::path default_socket_path();
    static std::filesystem::path default_pid_path();

private:
    class WorkerPool;
    class Connection;

    void request_stop() noexcept;
    void accept_connections();
    void close_connections() noexcept;
    void cleanup_endpoint() noexcept;

    UnixServerOptions options_;
    std::shared_ptr<CoreRuntime> runtime_;
    std::unique_ptr<SessionManager> sessions_;
    std::unique_ptr<WorkerPool> workers_;
    int listen_fd_ = -1;
    std::filesystem::path socket_path_;
    std::filesystem::path pid_path_;
    bool endpoint_owned_ = false;
    bool pid_owned_ = false;
    std::atomic_bool stopping_{false};

    std::mutex connections_mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::vector<std::thread> connection_threads_;
};

}  // namespace ime::unix_service
