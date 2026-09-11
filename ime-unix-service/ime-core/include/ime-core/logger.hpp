#pragma once

#include <functional>
#include <string>

namespace llavon::ime::core {

class Logger {
public:
    // std::function instead of std::move_only_function: libc++ (Apple
    // toolchains) does not implement P0288.
    using MessageFactory = std::function<std::string()>;

    virtual ~Logger() = default;

    // Submits an already materialized message. Implementations must not block
    // the caller and must not throw.
    virtual void log(std::string message) noexcept = 0;

    // Submits a lazily materialized message. Implementations must never invoke
    // the factory on the calling thread. If logging is disabled or the message
    // cannot be accepted, the factory must not be invoked at all. Accepted
    // factories may be invoked at most once on the logger's worker thread.
    virtual void log(MessageFactory make_message) noexcept = 0;
};

}  // namespace llavon::ime::core
