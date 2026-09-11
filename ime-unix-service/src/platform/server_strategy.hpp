#pragma once

#include <memory>

namespace ime::unix_service {
struct UnixServerOptions;
}

namespace ime::unix_service {

class ServerStrategy {
public:
    virtual ~ServerStrategy() = default;
    virtual const char* name() const = 0;
    virtual int run() = 0;
};

std::unique_ptr<ServerStrategy> create_server_strategy();
std::unique_ptr<ServerStrategy> create_server_strategy(UnixServerOptions options);

}  // namespace ime::unix_service
