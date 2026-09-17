#include "fcitx5/prediction_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>
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
    const auto temp_dir = std::filesystem::temp_directory_path();
    const auto socket_path = temp_dir /
                             ("llavon-ime-coordinator-integration-" + std::to_string(getpid()) + ".sock");
    std::error_code filesystem_error;
    std::filesystem::remove(socket_path, filesystem_error);
    UnixSocketServer server;
    server.bind_listen(socket_path);

    protocol::ServiceEpoch epoch{};
    epoch[0] = 0x42;
    protocol::SessionId session_id{};
    session_id[0] = 0x19;
    bool server_ok = true;
    std::atomic<int> server_stage = 0;
    std::mutex close_mutex;
    std::condition_variable close_condition;
    bool close_received = false;
    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            server_stage = 1;
            const auto status = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status)) server_ok = false;
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open)) server_ok = false;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{session_id, epoch}}));

            const auto first_message = protocol::decode(receive_frame(connection));
            const auto* first = std::get_if<protocol::PredictRequest>(&first_message);
            if (first == nullptr || first->request_id != 1 || first->padding.size() != 1) {
                server_ok = false;
                return;
            }
            protocol::Error model_error;
            model_error.code = protocol::ErrorCode::ModelError;
            model_error.session_id = session_id;
            model_error.request_id = first->request_id;
            model_error.buffer_revision = first->buffer_revision;
            connection.send_all(protocol::encode(protocol::Message{model_error}));

            const auto second_message = protocol::decode(receive_frame(connection));
            const auto* second = std::get_if<protocol::PredictRequest>(&second_message);
            if (second == nullptr || second->request_id != 2 || second->padding.size() != 1) {
                server_ok = false;
                return;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::Prediction{session_id, second->request_id, second->buffer_revision, {{U'擬'}}}}));

            const auto close_message = protocol::decode(receive_frame(connection));
            const auto* close = std::get_if<protocol::CloseSessionRequest>(&close_message);
            if (close == nullptr || close->session_id != session_id) {
                server_ok = false;
                return;
            }
            connection.send_all(
                protocol::encode(protocol::Message{protocol::CloseSessionResponse{session_id}}));
            {
                std::lock_guard lock(close_mutex);
                close_received = true;
            }
            close_condition.notify_one();
        } catch (...) {
            server_ok = false;
        }
    });

    FallbackEngine fallback(IME_FCITX5_TEST_TABLE_PATH);
    MixedInputDecoder decoder([&fallback](std::u16string_view reading) { return fallback.lookup(reading); },
                              [&fallback](std::u16string_view word) { return fallback.latin_frequency(word); });
    const auto overrides_path = temp_dir /
                                ("llavon-ime-coordinator-integration-overrides-" + std::to_string(getpid()) + ".txt");
    PhraseOverrideStore overrides(overrides_path);
    InputProcessor processor(fallback, decoder, overrides);
    ServiceTransportOptions options;
    options.socket_path = socket_path;
    options.auto_start = false;
    ServiceTransport transport(options);
    Config config = default_config();
    InputSession session;
    for (const char32_t key : std::u32string(U"su3")) {
        if (!session.buffer.add_bopomofo_key(key, BopomofoKeyboardLayout::Standard)) return EXIT_FAILURE;
    }

    auto alive = std::make_shared<bool>(true);
    std::mutex dispatch_mutex;
    std::condition_variable dispatch_condition;
    std::queue<std::function<void(fcitx::InputContext*)>> dispatch_queue;
    int redraw_count = 0;
    bool fallback_applied = false;
    bool prediction_applied = false;
    PredictionCoordinator coordinator(
        transport, processor,
        [&](PredictionCoordinator::ContextReference,
            std::function<void(fcitx::InputContext*)> body) {
            {
                std::lock_guard lock(dispatch_mutex);
                dispatch_queue.push(std::move(body));
            }
            dispatch_condition.notify_one();
        },
        alive,
        PredictionCoordinator::Callbacks{
            [&config]() -> const Config& { return config; },
            [](fcitx::InputContext*, InputSession&) {},
            [&session](fcitx::InputContext*, const std::function<void(InputSession&)>& body) { body(session); },
            [&](fcitx::InputContext*) {
                ++redraw_count;
                const auto* candidates = session.buffer.segment_candidates(0);
                if (redraw_count == 1) {
                    fallback_applied = candidates != nullptr && !candidates->empty();
                } else if (redraw_count == 2) {
                    prediction_applied = candidates != nullptr && !candidates->empty() && candidates->front() == U'擬';
                }
            },
            [](fcitx::InputContext*) -> ImeInputContextProperty* { return nullptr; },
        });

    coordinator.request(nullptr, session);
    // A second request while open/predict is in flight must become a dirty
    // retry after the first error response is handled.
    coordinator.request(nullptr, session);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (redraw_count != 2) {
        std::function<void(fcitx::InputContext*)> body;
        {
            std::unique_lock lock(dispatch_mutex);
            if (!dispatch_condition.wait_until(lock, deadline, [&]() { return !dispatch_queue.empty(); })) {
                alive.reset();
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
            body = std::move(dispatch_queue.front());
            dispatch_queue.pop();
        }
        body(nullptr);
    }

    coordinator.close_session(session);
    {
        std::unique_lock lock(close_mutex);
        if (!close_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return close_received; })) {
            alive.reset();
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
    transport.stop();
    server_thread.join();
    std::filesystem::remove(socket_path, filesystem_error);
    std::filesystem::remove(overrides_path, filesystem_error);

    if (!server_ok || !fallback_applied || !prediction_applied || session.prediction.pending ||
        session.prediction.session_open() ||
        session.prediction.next_request_id != 3) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
