#include "engine/stderr_logger.hpp"

#include <iostream>
#include <utility>

namespace ime::unix_service {

StderrLogger::StderrLogger(std::size_t max_queue) : max_queue_(max_queue) {
    worker_ = std::jthread([this](std::stop_token stop) { worker_loop(std::move(stop)); });
}

StderrLogger::~StderrLogger() {
    {
        std::lock_guard lock(mutex_);
        worker_.request_stop();
    }
    cv_.notify_all();
}

void StderrLogger::log(std::string message) noexcept {
    try {
        {
            std::lock_guard lock(mutex_);
            if (worker_.get_stop_token().stop_requested() || queue_.size() >= max_queue_) {
                return;
            }
            queue_.emplace_back(std::move(message));
        }
        cv_.notify_one();
    } catch (...) {
        // Never throw from the logger, even on allocation failure.
    }
}

void StderrLogger::log(MessageFactory make_message) noexcept {
    try {
        {
            std::lock_guard lock(mutex_);
            if (worker_.get_stop_token().stop_requested() || queue_.size() >= max_queue_) {
                return;
            }
            queue_.emplace_back(std::move(make_message));
        }
        cv_.notify_one();
    } catch (...) {
        // The factory is destroyed without being evaluated, which is allowed.
    }
}

void StderrLogger::worker_loop(std::stop_token stop) {
    while (true) {
        std::variant<std::string, MessageFactory> item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop.stop_requested()) {
                    break;
                }
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        std::string message;
        if (auto* text = std::get_if<std::string>(&item)) {
            message = std::move(*text);
        } else if (auto* factory = std::get_if<MessageFactory>(&item)) {
            try {
                message = (*factory)();
            } catch (...) {
                message = "[CORE] logger: message factory threw";
            }
        }

        try {
            std::clog << message << '\n' << std::flush;
        } catch (...) {
            // std::clog is not expected to throw, but the worker must survive.
        }
    }
}

}  // namespace ime::unix_service
