#include "engine_harness.hpp"

#include <fcitx-utils/macros.h>
#include <fcitx-utils/testing.h>
#include <fcitx/instance.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>

#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

using namespace ime::fcitx5;
using namespace ime::fcitx5::test;

namespace {

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
    const auto config_home = std::filesystem::path(TESTING_BINARY_DIR) / "prediction-integration-config";
    std::filesystem::remove_all(config_home);
    std::filesystem::create_directories(config_home / "conf");
    std::filesystem::create_directories(config_home / "fcitx5" / "conf");
    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("llavon-ime-engine-prediction-" + std::to_string(getpid()) + ".sock");

    setenv("FCITX_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    setenv("IME_FCITX5_TABLE_PATH", TESTING_TABLE_PATH, 1);
    setenv("LLAVON_IME_UNIX_SOCKET_PATH", socket_path.c_str(), 1);
    setenv("IME_FCITX5_DISABLE_SERVICE", "1", 1);

    UnixSocketServer server;
    server.bind_listen(socket_path);
    std::atomic<bool> server_ok = true;
    std::atomic<int> server_stage = 0;
    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            server_stage = 1;
            protocol::ServiceEpoch epoch{};
            epoch[0] = 0x53;
            protocol::SessionId session_id{};
            session_id[0] = 0x27;

            const auto status = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status)) server_ok = false;
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open)) server_ok = false;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{session_id, epoch}}));

            const auto predict_message = protocol::decode(receive_frame(connection));
            const auto* predict = std::get_if<protocol::PredictRequest>(&predict_message);
            if (predict == nullptr || predict->padding.size() != 1 || predict->context != u"history") {
                server_ok = false;
                return;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::Prediction{session_id, predict->request_id, predict->buffer_revision, {{U'擬'}}}}));
            server_stage = 2;

            const auto close_message = protocol::decode(receive_frame(connection));
            const auto* close = std::get_if<protocol::CloseSessionRequest>(&close_message);
            if (close == nullptr || close->session_id != session_id) {
                server_ok = false;
                return;
            }
            connection.send_all(
                protocol::encode(protocol::Message{protocol::CloseSessionResponse{session_id}}));
            server_stage = 3;
        } catch (...) {
            server_ok = false;
        }
    });

    bool prediction_visible = false;
    {
        fcitx::setupTestingEnvironment(TESTING_BINARY_DIR, {TESTING_BINARY_DIR},
                                       {TESTING_BINARY_DIR "/tests/test"});
        char arg0[] = "test-engine-prediction";
        char arg1[] = "--disable=all";
        char arg2[] = "--enable=testim,testfrontend,llavon-ime";
        char* argv[] = {arg0, arg1, arg2};
        fcitx::Instance instance(FCITX_ARRAY_SIZE(argv), argv);
        instance.addonManager().registerDefaultLoader(nullptr);

        std::shared_ptr<EngineHarness> harness;
        std::atomic<bool> done = false;
        instance.eventDispatcher().schedule([&]() {
            harness = std::make_shared<EngineHarness>(&instance);
            harness->set_config("SmartEnglish", "False");
            harness->set_surrounding("history", 7, 7);
            harness->type("su3");
        });

        std::thread observer([&]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline && !done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                instance.eventDispatcher().schedule([&]() {
                    if (done || !harness) return;
                    auto* state = harness->engine_state();
                    const auto* candidates =
                        state == nullptr ? nullptr : state->session.buffer.segment_candidates(0);
                    if (candidates != nullptr && !candidates->empty() && candidates->front() == U'擬') {
                        prediction_visible = true;
                        harness.reset();
                        done = true;
                        instance.exit();
                    }
                });
            }
            if (!done) {
                instance.eventDispatcher().schedule([&]() {
                    harness.reset();
                    done = true;
                    instance.exit();
                });
            }
        });

        instance.exec();
        done = true;
        observer.join();
        harness.reset();
    }

    if (server_stage == 0) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket_path);
        } catch (...) {
        }
    }
    server_thread.join();
    return prediction_visible && server_ok && server_stage == 3 ? EXIT_SUCCESS : EXIT_FAILURE;
}
