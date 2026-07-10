#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ime::fcitx5 {

class UnixSocketConnection {
public:
    UnixSocketConnection() = default;
    explicit UnixSocketConnection(int fd);
    ~UnixSocketConnection();

    UnixSocketConnection(const UnixSocketConnection&) = delete;
    UnixSocketConnection& operator=(const UnixSocketConnection&) = delete;
    UnixSocketConnection(UnixSocketConnection&& other) noexcept;
    UnixSocketConnection& operator=(UnixSocketConnection&& other) noexcept;

    bool valid() const noexcept;
    void send_all(std::span<const std::uint8_t> bytes) const;
    std::vector<std::uint8_t> recv_exact(size_t size) const;

private:
    void close();

    int fd_ = -1;
};

class UnixSocketClient {
public:
    UnixSocketConnection connect(const std::filesystem::path& path) const;
};

class UnixSocketServer {
public:
    UnixSocketServer() = default;
    ~UnixSocketServer();

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    void bind_listen(const std::filesystem::path& path);
    UnixSocketConnection accept_one() const;
    int native_handle() const noexcept;

private:
    int fd_ = -1;
    std::filesystem::path path_;
};

}  // namespace ime::fcitx5
