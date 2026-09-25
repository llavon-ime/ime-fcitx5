#pragma once

#include "host/host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace llavon::ime::test {

// Host test double: records commits and redraws, returns a configurable
// context, and queues post() bodies so tests can run them on the thread that
// owns the engine (mirroring a real host's main loop).
class FakeHost final : public Host {
public:
    void post(std::function<void()> body) override {
        std::lock_guard lock(mutex_);
        queue_.push(std::move(body));
        condition_.notify_all();
    }

    void commit(ContextId context, std::u16string_view text) override {
        std::lock_guard lock(mutex_);
        commits_.emplace_back(context, std::u16string(text));
        condition_.notify_all();
    }

    void update_ui(ContextId context) override {
        std::lock_guard lock(mutex_);
        ++redraw_count_;
        last_redraw_context_ = context;
        condition_.notify_all();
    }

    HostContext surrounding_text(ContextId) override {
        std::lock_guard lock(mutex_);
        return surrounding_;
    }

    bool is_sensitive(ContextId) override {
        std::lock_guard lock(mutex_);
        return sensitive_;
    }

    bool inject_probe(ContextId context, std::u16string_view token) override {
        std::lock_guard lock(mutex_);
        injected_.emplace_back(context, std::u16string(token));
        return inject_ok_;
    }

    void remove_probe(ContextId context, std::size_t units) override {
        std::lock_guard lock(mutex_);
        removals_.emplace_back(context, units);
    }

    std::vector<int> probe_processes(ContextId) override {
        std::lock_guard lock(mutex_);
        return probe_pids_;
    }

    void set_inject_ok(bool ok) {
        std::lock_guard lock(mutex_);
        inject_ok_ = ok;
    }

    void set_probe_pids(std::vector<int> pids) {
        std::lock_guard lock(mutex_);
        probe_pids_ = std::move(pids);
    }

    std::vector<std::pair<ContextId, std::u16string>> injected() const {
        std::lock_guard lock(mutex_);
        return injected_;
    }

    std::vector<std::pair<ContextId, std::size_t>> removals() const {
        std::lock_guard lock(mutex_);
        return removals_;
    }

    void set_surrounding(HostContext context) {
        std::lock_guard lock(mutex_);
        surrounding_ = std::move(context);
    }

    void set_sensitive(bool sensitive) {
        std::lock_guard lock(mutex_);
        sensitive_ = sensitive;
    }

    int redraw_count() const {
        std::lock_guard lock(mutex_);
        return redraw_count_;
    }

    ContextId last_redraw_context() const {
        std::lock_guard lock(mutex_);
        return last_redraw_context_;
    }

    std::vector<std::pair<ContextId, std::u16string>> commits() const {
        std::lock_guard lock(mutex_);
        return commits_;
    }

    bool has_pending_posts() const {
        std::lock_guard lock(mutex_);
        return !queue_.empty();
    }

    // Runs queued post() bodies on the calling thread until `predicate` holds
    // or the timeout elapses. The predicate is evaluated without holding the
    // internal lock.
    template <typename Predicate>
    bool pump_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (predicate()) return true;
            std::function<void()> body;
            {
                std::unique_lock lock(mutex_);
                if (queue_.empty()) {
                    if (!condition_.wait_until(lock, deadline, [&]() { return !queue_.empty(); })) {
                        return predicate();
                    }
                }
                body = std::move(queue_.front());
                queue_.pop();
            }
            body();
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::pair<ContextId, std::u16string>> commits_;
    HostContext surrounding_;
    bool sensitive_ = false;
    bool inject_ok_ = false;
    std::vector<int> probe_pids_;
    std::vector<std::pair<ContextId, std::u16string>> injected_;
    std::vector<std::pair<ContextId, std::size_t>> removals_;
    int redraw_count_ = 0;
    ContextId last_redraw_context_ = 0;
};

}  // namespace llavon::ime::test
