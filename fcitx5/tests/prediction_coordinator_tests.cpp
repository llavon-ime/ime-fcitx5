#ifndef _WIN32

#include "fcitx5/prediction_coordinator.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "bopomofo/keymap.hpp"
#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "input/input_processor.hpp"
#include "ipc/unix_socket.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include "protocol/protocol.hpp"

namespace {

using namespace ime::fcitx5;

protocol::ByteVector receive_frame(const UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

}  // namespace

int main() {
    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("llavon-ime-coordinator-test-" + std::to_string(getpid()) + ".sock");
    std::error_code error;
    std::filesystem::remove(socket_path, error);

    UnixSocketServer server;
    server.bind_listen(socket_path);
    std::mutex mutex;
    std::condition_variable condition;
    bool open_received = false;
    bool server_ok = true;
    std::atomic<int> server_stage = 0;
    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            server_stage = 1;
            const auto status = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status)) server_ok = false;
            protocol::ServiceEpoch epoch{};
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open)) server_ok = false;
            {
                std::lock_guard lock(mutex);
                open_received = true;
            }
            condition.notify_one();

            // Keep the request in flight until ServiceTransport::stop()
            // disconnects it. The callback then runs after the coordinator has
            // deliberately been destroyed below.
            (void)connection.recv_exact(1);
        } catch (...) {
            // The transport closing the blocked connection is expected.
        }
    });

    FallbackEngine fallback(IME_FCITX5_TEST_TABLE_PATH);
    MixedInputDecoder decoder([&fallback](std::u16string_view reading) { return fallback.lookup(reading); },
                              [&fallback](std::u16string_view word) { return fallback.latin_frequency(word); });
    const auto overrides_path = socket_path.parent_path() /
                                ("llavon-ime-coordinator-overrides-" + std::to_string(getpid()) + ".txt");
    PhraseOverrideStore overrides(overrides_path);
    InputProcessor processor(fallback, decoder, overrides);
    ServiceTransportOptions options;
    options.socket_path = socket_path;
    options.auto_start = false;
    ServiceTransport transport(options);
    Config config = default_config();
    auto alive = std::make_shared<bool>(true);

    std::atomic<int> dispatch_count = 0;
    auto coordinator = std::make_unique<PredictionCoordinator>(
        transport, processor,
        [&dispatch_count](PredictionCoordinator::ContextReference,
                          std::function<void(fcitx::InputContext*)>) { ++dispatch_count; },
        alive,
        PredictionCoordinator::Callbacks{
            [&config]() -> const Config& { return config; },
            [](fcitx::InputContext*, InputSession&) {},
            [](fcitx::InputContext*, const std::function<void(InputSession&)>&) {},
            [](fcitx::InputContext*) {},
            [](fcitx::InputContext*) -> ImeInputContextProperty* { return nullptr; },
        });

    InputSession session;
    for (const char32_t key : std::u32string(U"su3")) {
        if (!session.buffer.add_bopomofo_key(key, BopomofoKeyboardLayout::Standard)) return EXIT_FAILURE;
    }
    coordinator->request(nullptr, session);
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(2), [&]() { return open_received; })) {
            transport.stop();
            if (server_stage == 0) {
                try {
                    UnixSocketClient client;
                    (void)client.connect(socket_path);
                } catch (...) {
                }
            }
            server_thread.join();
            return EXIT_FAILURE;
        }
    }

    alive.reset();
    coordinator.reset();
    transport.stop();
    if (server_stage == 0) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket_path);
        } catch (...) {
        }
    }
    server_thread.join();
    std::filesystem::remove(socket_path, error);
    std::filesystem::remove(overrides_path, error);
    return server_ok && dispatch_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else

int main() { return EXIT_SUCCESS; }

#endif
