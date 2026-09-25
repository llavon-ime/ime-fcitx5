#include "context/memory_context.hpp"
#include "host/engine.hpp"

#include "fake_host.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace llavon::ime {
namespace {

using llavon::ime::test::FakeHost;

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

// A fake helper script that answers like llavon-ime-memscan.
class FakeHelper {
public:
    FakeHelper(const std::string& body, const char* name) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("llavon-ime-memscan-" + std::string(name) + "-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)) + ".sh");
        std::ofstream script(path_);
        script << "#!/bin/sh\n" << body << "\n";
        script.close();
        std::filesystem::permissions(path_,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);
    }

    ~FakeHelper() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct ProbeState {
    std::mutex mutex;
    int inject_calls = 0;
    int remove_calls = 0;
    bool inject_ok = true;
    bool sensitive = false;
    std::u16string last_token;

    MemoryProbeCallbacks callbacks() {
        MemoryProbeCallbacks result;
        result.inject = [this](std::u16string_view token) {
            std::lock_guard lock(mutex);
            ++inject_calls;
            last_token = std::u16string(token);
            return inject_ok;
        };
        result.remove = [this](std::size_t) {
            std::lock_guard lock(mutex);
            ++remove_calls;
        };
        result.processes = [] { return std::vector<int>{4242}; };
        result.sensitive = [this] {
            std::lock_guard lock(mutex);
            return sensitive;
        };
        return result;
    }
};

bool test_probe_publishes_sample() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":true,\"before\":\"hello probe\",\"after\":\"\"}'",
                             "ok");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts with an executable helper");
    ok &= check(provider.availability().availability == AccessibilityAvailability::Available,
                "a working helper reports Available");
    provider.set_active(true);
    provider.refresh();
    ok &= check(wait_for([&] {
                    const auto sample = provider.latest();
                    return sample.has_value() && sample->usable;
                }),
                "the probe publishes a usable sample");
    const auto sample = provider.latest();
    ok &= check(sample && sample->text == u"hello probe", "the sample carries the text before the token");
    ok &= check(wait_for([&] {
                    std::lock_guard lock(state.mutex);
                    return state.remove_calls == 1;
                }),
                "the probe token is removed again");
    {
        std::lock_guard lock(state.mutex);
        ok &= check(state.inject_calls == 1, "exactly one injection per probe");
        ok &= check(state.last_token.size() == 16, "the token has 16 code units");
        for (const char16_t unit : state.last_token) {
            ok &= check(unit >= 0xE000 && unit <= 0xF8FF, "the token uses private-use code points");
        }
    }
    ok &= check(provider.probe_count() == 1, "one helper run is counted");
    provider.stop();
    return ok;
}

bool test_missing_helper_is_reported() {
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), "/nonexistent/llavon-ime-memscan");
    bool ok = check(!provider.start(), "start fails without the helper");
    const auto availability = provider.availability();
    ok &= check(availability.availability == AccessibilityAvailability::Unavailable,
                "the source reports Unavailable");
    ok &= check(availability.detail == "helper-missing", "the missing helper detail is reported");
    ok &= check(!provider.running(), "a refused provider is not running");
    return ok;
}

bool test_permission_denied_is_reported() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":false,\"error\":\"denied\"}'", "denied");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    provider.refresh();
    ok &= check(wait_for([&] {
                    return provider.availability().availability == AccessibilityAvailability::Unavailable;
                }),
                "a denied probe marks the source unavailable");
    ok &= check(provider.availability().detail == "permission-denied",
                "the permission detail is surfaced for the status line");
    const auto sample = provider.latest();
    ok &= check(sample && !sample->usable, "a denied probe publishes an unusable sample");
    ok &= check(wait_for([&] {
                    std::lock_guard lock(state.mutex);
                    return state.remove_calls == 1;
                }),
                "the token is removed even when the scan fails");
    provider.stop();
    return ok;
}

bool test_sensitive_and_inactive_skip() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":true,\"before\":\"x\"}'", "skip");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(false);
    provider.refresh();
    provider.set_active(true);
    {
        std::lock_guard lock(state.mutex);
        state.sensitive = true;
    }
    provider.refresh();
    {
        std::lock_guard lock(state.mutex);
        ok &= check(state.inject_calls == 0, "an inactive or sensitive context is never probed");
    }
    ok &= check(provider.probe_count() == 0, "no helper run happens");
    provider.stop();
    return ok;
}

bool test_probe_is_throttled() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":true,\"before\":\"x\"}'", "throttle");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    provider.refresh();
    ok &= check(wait_for([&] {
                    std::lock_guard lock(state.mutex);
                    return state.remove_calls == 1;
                }),
                "the first probe completes");
    provider.refresh();
    {
        std::lock_guard lock(state.mutex);
        ok &= check(state.inject_calls == 1, "an immediate second refresh is throttled");
    }
    provider.stop();
    return ok;
}

bool test_injection_failure() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":true,\"before\":\"x\"}'", "inject-fail");
    ProbeState state;
    {
        std::lock_guard lock(state.mutex);
        state.inject_ok = false;
    }
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    provider.refresh();
    const auto sample = provider.latest();
    ok &= check(sample && !sample->usable, "a client that cannot carry a probe publishes nothing");
    ok &= check(provider.probe_count() == 0, "no helper run happens without a token");
    provider.stop();
    return ok;
}

EngineOptions engine_options(const std::filesystem::path& helper) {
    EngineOptions options;
    options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    options.phrase_overrides_path =
        std::filesystem::temp_directory_path() / ("llavon-ime-memory-overrides-" + std::to_string(getpid()) + ".json");
    options.enable_accessibility = false;
    options.enable_memory_context = true;
    options.config.memory_context = true;
    options.transport.socket_path = options.phrase_overrides_path.parent_path() / "llavon-ime-no-service.sock";
    options.transport.auto_start = false;
    setenv("LLAVON_IME_MEMSCAN_PATH", helper.c_str(), 1);
    return options;
}

bool test_engine_probes_through_the_host() {
    const FakeHelper helper("printf '%s\\n' '{\"found\":true,\"before\":\"engine context\"}'", "engine");
    FakeHost host;
    host.set_inject_ok(true);
    host.set_probe_pids({4711});
    Engine engine(engine_options(helper.path()), host);
    const ContextId context = 7;
    engine.attach(context);
    engine.activate(context);

    bool ok = check(host.pump_until([&] { return host.removals().size() == 1; }),
                    "the engine injects a probe and removes it through the host");
    const auto injected = host.injected();
    ok &= check(injected.size() == 1 && injected.front().first == context,
                "the host received exactly one injection");
    if (!injected.empty()) {
        ok &= check(injected.front().second.size() == 16, "the injected token has 16 code units");
    }
    const auto removals = host.removals();
    if (!removals.empty()) {
        ok &= check(removals.front().second == 16, "the removal covers the injected token");
    }
    ok &= check(engine.memory_context_state().availability == AccessibilityAvailability::Available,
                "the engine reports the memory source as available");
    unsetenv("LLAVON_IME_MEMSCAN_PATH");
    return ok;
}

}  // namespace

}  // namespace llavon::ime

int run_memory_context_tests() {
    using namespace llavon::ime;
    if (!memory_context_supported()) {
        std::printf("memory context tests skipped (no probe backend on this platform)\n");
        return EXIT_SUCCESS;
    }
    bool ok = true;
    ok &= test_probe_publishes_sample();
    ok &= test_missing_helper_is_reported();
    ok &= test_permission_denied_is_reported();
    ok &= test_sensitive_and_inactive_skip();
    ok &= test_probe_is_throttled();
    ok &= test_injection_failure();
    ok &= test_engine_probes_through_the_host();
    if (ok) std::printf("memory context tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
