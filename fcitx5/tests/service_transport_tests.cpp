#ifndef _WIN32

#include "engine/service_transport.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {

ime::fcitx5::protocol::ByteVector receive_frame(const ime::fcitx5::UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

}  // namespace

int run_service_transport_tests() {
    using namespace ime::fcitx5;
    const auto socket_path = std::filesystem::temp_directory_path() / "llavon-ime-transport-test.sock";
    std::error_code error;
    std::filesystem::remove(socket_path, error);
    UnixSocketServer server;
    server.bind_listen(socket_path);

    protocol::ServiceEpoch epoch{};
    epoch[0] = 0x42;
    protocol::SessionId session{};
    session[0] = 0x19;
    std::mutex mutex;
    std::condition_variable condition;
    bool server_ok = true;
    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            const auto status_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status_request)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open_request)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));

            const auto predict_request = protocol::decode(receive_frame(connection));
            const auto* request = std::get_if<protocol::PredictRequest>(&predict_request);
            if (request == nullptr || request->session_id != session || request->request_id != 1) {
                server_ok = false;
            } else {
                connection.send_all(protocol::encode(protocol::Message{protocol::Prediction{session, 1, request->buffer_revision, {{U'你'}}}}));
            }
        } catch (...) {
            server_ok = false;
        }
        condition.notify_one();
    });

    ServiceTransportOptions options;
    options.socket_path = socket_path;
    options.auto_start = false;
    ServiceTransport transport(options);
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    std::vector<protocol::Message> responses;
    transport.open_session([&](protocol::Message response) {
        std::lock_guard lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    {
        std::unique_lock lock(callback_mutex);
        if (!callback_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return responses.size() >= 1; })) {
            transport.stop();
            server_thread.join();
            return EXIT_FAILURE;
        }
    }
    if (!std::holds_alternative<protocol::OpenSessionResponse>(responses.front())) {
        transport.stop();
        server_thread.join();
        return EXIT_FAILURE;
    }
    transport.predict(session, 1, 7, {}, {{false, u"ㄋㄧˇ", 0}}, [&](protocol::Message response) {
        std::lock_guard lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    {
        std::unique_lock lock(callback_mutex);
        if (!callback_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return responses.size() >= 2; })) {
            transport.stop();
            server_thread.join();
            return EXIT_FAILURE;
        }
    }
    transport.stop();
    server_thread.join();
    if (!server_ok || !std::holds_alternative<protocol::Prediction>(responses.back())) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

#else

int run_service_transport_tests() { return EXIT_SUCCESS; }

#endif
