#pragma once

#include <memory>

#if !defined(_WIN32)
namespace imesvc {
struct UnixServerOptions;
}
#endif

namespace imesvc {

class ServerStrategy {
public:
    virtual ~ServerStrategy() = default;
    virtual const char* name() const = 0;
    virtual int run() = 0;
};

std::unique_ptr<ServerStrategy> create_server_strategy();

#if !defined(_WIN32)
std::unique_ptr<ServerStrategy> create_server_strategy(UnixServerOptions options);
#endif

}  // namespace imesvc
