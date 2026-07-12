#pragma once

#include "protocol/protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

namespace ime::fcitx5 {

struct ServiceTransportOptions {
    std::filesystem::path socket_path;
    std::filesystem::path service_path;
    std::filesystem::path model_path;
    std::filesystem::path tables_dir;
    std::uint32_t context_length = 512;
    std::uint32_t threads = 8;
    int gpu_layers = -2;
    std::size_t max_sessions = 8;
    std::size_t max_idle_sessions = 4;
    std::size_t max_concurrent_predictions = 2;
    std::uint32_t idle_timeout_seconds = 1800;
    bool auto_start = true;
};

class ServiceTransport final {
public:
    using Callback = std::function<void(protocol::Message)>;

    explicit ServiceTransport(ServiceTransportOptions options = {});
    ~ServiceTransport();

    ServiceTransport(const ServiceTransport&) = delete;
    ServiceTransport& operator=(const ServiceTransport&) = delete;

    void open_session(Callback callback);
    void predict(const protocol::SessionId& session_id, std::uint64_t request_id, std::uint64_t buffer_revision,
                 std::u16string context, std::vector<protocol::PaddingEntry> padding, Callback callback);
    void close_session(const protocol::SessionId& session_id, Callback callback);
    void status(std::optional<protocol::SessionId> session_id, Callback callback);
    void shutdown(Callback callback);

    void stop();
    bool connected() const noexcept;
    std::optional<protocol::ServiceEpoch> service_epoch() const;
    const ServiceTransportOptions& options() const noexcept;

    static std::filesystem::path default_socket_path();
    static std::filesystem::path default_service_path();

private:
    enum class RequestKind : std::uint8_t { Open, Predict, Close, Status, Shutdown };
    struct Pending {
        RequestKind kind;
        protocol::Message message;
        Callback callback;
    };

    void enqueue(RequestKind kind, protocol::Message message, Callback callback);
    void run();
    bool ensure_connected();
    void disconnect() noexcept;
    void fail(Pending pending, protocol::ErrorCode code, std::string message);
    static bool matches(RequestKind kind, const protocol::Message& request, const protocol::Message& response);
    static protocol::Error correlation_error(const protocol::Message& request, protocol::ErrorCode code, std::string message);
    void observe_response(const protocol::Message& response);

    ServiceTransportOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Pending> queue_;
    std::optional<protocol::ServiceEpoch> epoch_;
    bool stopping_ = false;
    bool connected_ = false;
    int socket_fd_ = -1;
    std::thread worker_;
};

}  // namespace ime::fcitx5
