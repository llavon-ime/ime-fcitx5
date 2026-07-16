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

bool stop_during_negotiation_test() {
    using namespace ime::fcitx5;
    const auto socket_path = std::filesystem::temp_directory_path() / "llavon-ime-transport-stop-test.sock";
    std::error_code error;
    std::filesystem::remove(socket_path, error);
    UnixSocketServer server;
    server.bind_listen(socket_path);
    std::mutex mutex;
    std::condition_variable condition;
    bool accepted = false;
    bool release = false;
    std::thread server_thread([&]() {
        auto connection = server.accept_one();
        (void)receive_frame(connection);
        {
            std::lock_guard lock(mutex);
            accepted = true;
        }
        condition.notify_one();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&]() { return release; });
    });

    ServiceTransportOptions options;
    options.socket_path = socket_path;
    options.auto_start = false;
    ServiceTransport transport(options);
    transport.open_session({});
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(2), [&]() { return accepted; })) {
            release = true;
            lock.unlock();
            condition.notify_one();
            transport.stop();
            server_thread.join();
            return false;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    transport.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    server_thread.join();
    return elapsed < std::chrono::seconds(2);
}

bool legacy_service_rejection_test() {
    using namespace ime::fcitx5;
    const auto socket_path = std::filesystem::temp_directory_path() / "llavon-ime-transport-legacy-test.sock";
    std::error_code error;
    std::filesystem::remove(socket_path, error);
    UnixSocketServer server;
    server.bind_listen(socket_path);
    bool server_ok = true;
    std::thread server_thread([&]() {
        try {
            auto negotiation = server.accept_one();
            if (!std::holds_alternative<protocol::HelloRequest>(protocol::decode(receive_frame(negotiation)))) {
                server_ok = false;
            }
            protocol::Error rejection;
            rejection.code = protocol::ErrorCode::ProtocolError;
            rejection.message = "unknown message type";
            negotiation.send_all(protocol::encode(protocol::Message{rejection}));

        } catch (...) {
            server_ok = false;
        }
    });

    ServiceTransportOptions options;
    options.socket_path = socket_path;
    options.auto_start = false;
    ServiceTransport transport(options);
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    bool rejected = false;
    transport.open_session([&](protocol::Message response) {
        {
            std::lock_guard lock(mutex);
            completed = true;
            rejected = std::holds_alternative<protocol::Error>(response);
        }
        condition.notify_one();
    });
    {
        std::unique_lock lock(mutex);
        (void)condition.wait_for(lock, std::chrono::seconds(2), [&]() { return completed; });
    }
    transport.stop();
    server_thread.join();
    return completed && rejected && server_ok;
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
            const auto hello_request = protocol::decode(receive_frame(connection));
            const auto* hello = std::get_if<protocol::HelloRequest>(&hello_request);
            if (hello == nullptr || hello->minimum_version > protocol::kProtocolVersion ||
                hello->maximum_version < protocol::kProtocolVersion) {
                server_ok = false;
            }
            connection.send_all(protocol::encode(protocol::Message{protocol::HelloResponse{
                protocol::kProtocolVersion, protocol::kSupportedCapabilities}}));
            const auto status_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status_request)) server_ok = false;
            protocol::StatusResponse status;
            status.service_epoch = epoch;
            status.max_sessions = 8;
            connection.send_all(protocol::encode(protocol::Message{status}));

            const auto open_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open_request)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));

            const auto predict_request = protocol::decode(receive_frame(connection));
            const auto* request = std::get_if<protocol::PredictRequest>(&predict_request);
            if (request == nullptr || request->session_id != session || request->request_id != 1) {
                server_ok = false;
            } else {
                protocol::Prediction prediction{session, 1, request->buffer_revision, {{U'你'}}};
                prediction.feedback_token[0] = 1;
                connection.send_all(protocol::encode(protocol::Message{prediction}));
            }

            if (!std::holds_alternative<protocol::TrainingStatusRequest>(protocol::decode(receive_frame(connection)))) {
                server_ok = false;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::TrainingStatusResponse{true, true, 3, 6, "lora-test"}}));
            if (!std::holds_alternative<protocol::DeletePersonalDataRequest>(protocol::decode(receive_frame(connection)))) {
                server_ok = false;
            }
            connection.send_all(protocol::encode(protocol::Message{protocol::DeletePersonalDataResponse{true}}));

            auto shutdown_connection = server.accept_one();
            const auto shutdown_hello = protocol::decode(receive_frame(shutdown_connection));
            if (!std::holds_alternative<protocol::HelloRequest>(shutdown_hello)) server_ok = false;
            shutdown_connection.send_all(protocol::encode(protocol::Message{
                protocol::HelloResponse{protocol::kProtocolVersion, protocol::kSupportedCapabilities}}));
            const auto shutdown_request = protocol::decode(receive_frame(shutdown_connection));
            if (!std::holds_alternative<protocol::ShutdownRequest>(shutdown_request)) server_ok = false;
        } catch (...) {
            server_ok = false;
        }
        condition.notify_one();
    });

    ServiceTransportOptions options;
    options.socket_path = socket_path;
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
    transport.training_status([&](protocol::Message response) {
        std::lock_guard lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    transport.delete_personal_data([&](protocol::Message response) {
        std::lock_guard lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    {
        std::unique_lock lock(callback_mutex);
        if (!callback_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return responses.size() >= 4; })) {
            transport.stop();
            server_thread.join();
            return EXIT_FAILURE;
        }
    }
    transport.stop();
    server_thread.join();
    if (!server_ok || !std::holds_alternative<protocol::TrainingStatusResponse>(responses[2]) ||
        !std::holds_alternative<protocol::DeletePersonalDataResponse>(responses[3]) ||
        !stop_during_negotiation_test() || !legacy_service_rejection_test()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

#else

int run_service_transport_tests() { return EXIT_SUCCESS; }

#endif
