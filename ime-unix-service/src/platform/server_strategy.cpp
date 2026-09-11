#include "server_strategy.hpp"

#include "unix_socket_server.hpp"

#include <utility>

namespace ime::unix_service {

std::unique_ptr<ServerStrategy> create_server_strategy() {
    return std::make_unique<UnixSocketServer>(UnixServerOptions{});
}

std::unique_ptr<ServerStrategy> create_server_strategy(UnixServerOptions options) {
    return std::make_unique<UnixSocketServer>(std::move(options));
}

}  // namespace ime::unix_service
