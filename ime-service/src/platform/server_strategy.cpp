#include "server_strategy.hpp"

#if defined(_WIN32)
#include "windows_named_pipe_server.hpp"
#else
#include "unix_socket_server.hpp"
#endif

#include <utility>

namespace imesvc {

std::unique_ptr<ServerStrategy> create_server_strategy() {
#if defined(_WIN32)
    return std::make_unique<WindowsNamedPipeServer>();
#else
    return std::make_unique<UnixSocketServer>(UnixServerOptions{});
#endif
}

#if !defined(_WIN32)
std::unique_ptr<ServerStrategy> create_server_strategy(UnixServerOptions options) {
    return std::make_unique<UnixSocketServer>(std::move(options));
}
#endif

}  // namespace imesvc
