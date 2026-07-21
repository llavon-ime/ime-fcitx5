#pragma once

#if defined(_WIN32)

#include <windows.h>
#include <sddl.h>

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../engine/llamaEngine.hpp"
#include "../pipe/server.hpp"
#include "server_strategy.hpp"

namespace imesvc {

namespace windows_named_pipe {

enum class PipeCommand : uint8_t {
    Predict = 1,
    ToggleInputMode = 2,
    GetInputMode = 3,
    Ready = 4,
};

enum class InputMode : uint8_t {
    Chinese = 0,
    English = 1,
};

enum class ServicePriority {
    Normal,
    AboveNormal,
    High,
    Realtime,
};

inline std::atomic<InputMode> g_input_mode{InputMode::Chinese};

inline InputMode current_input_mode() {
    return g_input_mode.load(std::memory_order_relaxed);
}

inline InputMode toggle_input_mode() {
    InputMode current = current_input_mode();
    while (true) {
        const InputMode next = current == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
        if (g_input_mode.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            return next;
        }
    }
}

inline std::unique_ptr<IEngine> create_engine() {
    return std::make_unique<LlamaEngine>();
}

inline ServicePriority selected_service_priority() {
    char env[32]{};
    const DWORD len = GetEnvironmentVariableA("LLAVON_IME_SERVICE_PRIORITY", env, static_cast<DWORD>(sizeof(env)));
    if (len == 0 || len >= sizeof(env)) {
        return ServicePriority::High;
    }

    const std::string_view value(env, len);
    if (value == "normal") {
        return ServicePriority::Normal;
    }
    if (value == "above_normal" || value == "above-normal") {
        return ServicePriority::AboveNormal;
    }
    if (value == "realtime" || value == "real-time") {
        return ServicePriority::Realtime;
    }
    return ServicePriority::High;
}

inline const char* priority_name(ServicePriority priority) {
    switch (priority) {
        case ServicePriority::Normal:
            return "normal";
        case ServicePriority::AboveNormal:
            return "above_normal";
        case ServicePriority::Realtime:
            return "realtime";
        case ServicePriority::High:
        default:
            return "high";
    }
}

inline void configure_windows_scheduling(ServicePriority priority) {
    DWORD priority_class = HIGH_PRIORITY_CLASS;
    int thread_priority = THREAD_PRIORITY_HIGHEST;

    switch (priority) {
        case ServicePriority::Normal:
            priority_class = NORMAL_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_NORMAL;
            break;
        case ServicePriority::AboveNormal:
            priority_class = ABOVE_NORMAL_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ServicePriority::Realtime:
            priority_class = REALTIME_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        case ServicePriority::High:
        default:
            priority_class = HIGH_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_HIGHEST;
            break;
    }

    if (!SetPriorityClass(GetCurrentProcess(), priority_class)) {
        std::cerr << "[WARN] SetPriorityClass failed: " << GetLastError() << std::endl;
    }
    if (!SetThreadPriority(GetCurrentThread(), thread_priority)) {
        std::cerr << "[WARN] SetThreadPriority failed: " << GetLastError() << std::endl;
    }

    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling))) {
        std::cerr << "[WARN] disabling process power throttling failed: " << GetLastError() << std::endl;
    }
}

inline asio::awaitable<bool> read_exact(asio::windows::stream_handle& pipe, void* buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        auto [ec, n] = co_await pipe.async_read_some(
            asio::buffer(static_cast<char*>(buf) + total, size - total), asio::as_tuple(asio::use_awaitable));
        if (ec || n == 0) co_return false;
        total += n;
    }
    co_return true;
}

template <typename T>
inline asio::awaitable<bool> read_val(asio::windows::stream_handle& pipe, T& val) {
    co_return co_await read_exact(pipe, &val, sizeof(T));
}

inline asio::awaitable<bool> write_exact(asio::windows::stream_handle& pipe, const void* buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        auto [ec, n] = co_await pipe.async_write_some(
            asio::buffer(static_cast<const char*>(buf) + total, size - total), asio::as_tuple(asio::use_awaitable));
        if (ec) co_return false;
        total += n;
    }
    co_return true;
}

template <typename T>
inline asio::awaitable<bool> write_val(asio::windows::stream_handle& pipe, const T& val) {
    co_return co_await write_exact(pipe, &val, sizeof(T));
}

inline asio::awaitable<std::vector<PaddingEntry>> read_padding(asio::windows::stream_handle& pipe) {
    uint32_t count = 0;
    if (!co_await read_val(pipe, count)) co_return std::vector<PaddingEntry>{};

    std::vector<PaddingEntry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t type = 0;
        if (!co_await read_val(pipe, type)) co_return std::vector<PaddingEntry>{};

        PaddingEntry e;
        if (type == 0) {
            uint32_t len = 0;
            if (!co_await read_val(pipe, len)) co_return std::vector<PaddingEntry>{};
            e.bpmf.resize(len);
            if (len > 0 && !co_await read_exact(pipe, e.bpmf.data(), len * sizeof(char16_t))) {
                co_return std::vector<PaddingEntry>{};
            }
            e.is_chosen = false;
        } else {
            if (!co_await read_val(pipe, e.chosen_char)) co_return std::vector<PaddingEntry>{};
            e.is_chosen = true;
        }
        entries.push_back(std::move(e));
    }
    co_return entries;
}

inline asio::awaitable<void> write_response(asio::windows::stream_handle& pipe,
                                            const std::vector<PredictResult>& results) {
    uint32_t count = static_cast<uint32_t>(results.size());
    if (!co_await write_val(pipe, count)) co_return;

    for (auto& r : results) {
        uint32_t nc = static_cast<uint32_t>(r.candidates.size());
        if (!co_await write_val(pipe, nc)) co_return;
        for (auto& [c, _] : r.candidates) {
            if (!co_await write_val(pipe, c)) co_return;
        }
    }
}

inline asio::awaitable<void> write_input_mode(asio::windows::stream_handle& pipe, InputMode mode) {
    const uint8_t raw_mode = static_cast<uint8_t>(mode);
    co_await write_val(pipe, raw_mode);
}

inline asio::awaitable<void> handle_client(asio::windows::stream_handle pipe) {
    std::unique_ptr<IEngine> engine;

    while (true) {
        uint8_t raw_command = 0;
        if (!co_await read_val(pipe, raw_command)) break;

        const auto command = static_cast<PipeCommand>(raw_command);
        if (command == PipeCommand::ToggleInputMode) {
            const InputMode mode = toggle_input_mode();
            std::clog << "[SRV] input mode: " << (mode == InputMode::Chinese ? "Chinese" : "English") << '\n';
            co_await write_input_mode(pipe, mode);
            continue;
        }

        if (command == PipeCommand::GetInputMode) {
            co_await write_input_mode(pipe, current_input_mode());
            continue;
        }

        if (!engine) {
            try {
                engine = create_engine();
            } catch (const std::exception& e) {
                std::cerr << "[ERR] engine init: " << e.what() << std::endl;
                co_return;
            }
        }

        if (command == PipeCommand::Ready) {
            uint8_t ok = 0;
            try {
                engine->ready();
                ok = 1;
            } catch (const std::exception& e) {
                std::cerr << "[ERR] ready: " << e.what() << std::endl;
            }
            co_await write_val(pipe, ok);
            continue;
        }

        if (command != PipeCommand::Predict) {
            std::cerr << "[ERR] unknown pipe command: " << static_cast<int>(raw_command) << std::endl;
            break;
        }

        uint32_t ctx_len = 0;
        if (!co_await read_val(pipe, ctx_len)) break;

        std::u16string context;
        if (ctx_len > 0) {
            context.resize(ctx_len);
            if (!co_await read_exact(pipe, context.data(), ctx_len * sizeof(char16_t))) break;
        }

        auto padding = co_await read_padding(pipe);
        if (padding.empty() && ctx_len == 0) break;

        std::vector<PredictResult> results;
        bool ok = false;
        try {
            results = engine->predict(context, padding);
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << "[ERR] predict: " << e.what() << std::endl;
        }
        if (ok) {
            co_await write_response(pipe, results);
        } else {
            uint32_t zero = 0;
            co_await write_val(pipe, zero);
        }
    }
}

inline asio::awaitable<void> listener(asio::io_context& io_ctx) {
    auto executor = co_await asio::this_coro::executor;

    while (true) {
        // SearchHost and other modern Windows text controls run in an
        // AppContainer.  The default pipe DACL does not grant AppContainer
        // tokens access, so those hosts otherwise receive ERROR_ACCESS_DENIED
        // and block their TSF UI thread while the frontend retries.
        PSECURITY_DESCRIPTOR security_descriptor = nullptr;
        constexpr wchar_t pipe_sddl[] =
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                pipe_sddl, SDDL_REVISION_1, &security_descriptor, nullptr)) {
            std::cerr << "[ERR] pipe security descriptor failed: " << GetLastError() << std::endl;
            co_return;
        }
        SECURITY_ATTRIBUTES security_attributes{};
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor = security_descriptor;

        HANDLE hPipe = CreateNamedPipeW(
            pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &security_attributes);
        LocalFree(security_descriptor);

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERR] CreateNamedPipe failed: " << GetLastError() << std::endl;
            co_return;
        }

        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(hPipe, &ol);
        DWORD err = GetLastError();

        if (!connected && err == ERROR_PIPE_CONNECTED) {
            CloseHandle(ol.hEvent);
        } else if (!connected && err == ERROR_IO_PENDING) {
            asio::windows::object_handle ev(executor);
            ev.assign(ol.hEvent);
            co_await ev.async_wait(asio::use_awaitable);
        } else {
            std::cerr << "[ERR] ConnectNamedPipe failed: " << err << std::endl;
            CloseHandle(ol.hEvent);
            CloseHandle(hPipe);
            continue;
        }

        std::clog << "[SRV] client connected\n";

        asio::windows::stream_handle stream(executor, hPipe);
        co_spawn(executor, handle_client(std::move(stream)), asio::detached);
    }
}

}  // namespace windows_named_pipe

class WindowsNamedPipeServer final : public ServerStrategy {
public:
    const char* name() const override {
        return "windows-named-pipe";
    }

    int run() override {
        SetConsoleOutputCP(65001);
        const auto priority = windows_named_pipe::selected_service_priority();
        windows_named_pipe::configure_windows_scheduling(priority);
        std::clog << "[SRV] service priority: " << windows_named_pipe::priority_name(priority) << '\n';
        std::clog << "[SRV] engine backend: llama\n";

        try {
            (void)legacy_llama_resources();
            std::clog << "[SRV] model loaded\n";
        } catch (const std::exception& e) {
            std::cerr << "[ERR] model load failed: " << e.what() << std::endl;
            return 1;
        }

        asio::io_context io_ctx;
        co_spawn(io_ctx, windows_named_pipe::listener(io_ctx), asio::detached);
        io_ctx.run();

        return 0;
    }
};

}  // namespace imesvc

#endif
