#include "protocol/binary_codec.hpp"
#include "protocol/protocol.hpp"
#include "ipc/unix_socket.hpp"
#include "text/utf.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int run_protocol_tests() {
    bool ok = true;

    ime::fcitx5::PredictRequest req;
    req.context = u"你好";
    req.padding.push_back({false, u"ㄋㄧˇ", 0});
    const auto bytes = ime::fcitx5::encode_message(req);
    const auto decoded = ime::fcitx5::decode_predict_request(bytes);
    ok = ok && decoded.context == req.context;
    ok = ok && decoded.padding.size() == 1;
    ok = ok && decoded.padding[0].bopomofo == u"ㄋㄧˇ";

    ime::fcitx5::StatusResponse status;
    status.running = true;
    status.backend = "fallback";
    const auto status_bytes = ime::fcitx5::encode_message(status);
    const auto decoded_status = ime::fcitx5::decode_status_response(status_bytes);
    ok = ok && decoded_status.running;
    ok = ok && decoded_status.backend == "fallback";

    ime::fcitx5::ControlRequest stop_request;
    stop_request.operation = "stop";
    const auto stop_bytes = ime::fcitx5::encode_message(stop_request);
    ok = ok && ime::fcitx5::decode_control_request(stop_bytes).operation == "stop";

    ime::fcitx5::PredictResponse response;
    response.candidates.push_back({U'你', U'妳'});
    const auto response_bytes = ime::fcitx5::encode_message(response);
    const auto decoded_response = ime::fcitx5::decode_predict_response(response_bytes);
    ok = ok && decoded_response.candidates.size() == 1;
    ok = ok && decoded_response.candidates.front().size() == 2;
    ok = ok && decoded_response.candidates.front().front() == U'你';

    auto malformed = bytes;
    malformed.push_back(0);
    bool rejected_extra_payload = false;
    try {
        (void)ime::fcitx5::decode_predict_request(malformed);
    } catch (...) {
        rejected_extra_payload = true;
    }
    ok = ok && rejected_extra_payload;

    req.padding[0].chosen = true;
    req.padding[0].chosen_char = U'你';
    const auto chosen_bytes = ime::fcitx5::encode_message(req);
    ok = ok && ime::fcitx5::decode_predict_request(chosen_bytes).padding[0].chosen;
    ok = ok && ime::fcitx5::decode_predict_request(chosen_bytes).padding[0].chosen_char == U'你';

    bool rejected_empty_chosen_char = false;
    try {
        ime::fcitx5::PredictRequest invalid_request;
        invalid_request.padding.push_back({true, {}, 0});
        (void)ime::fcitx5::encode_message(invalid_request);
    } catch (...) {
        rejected_empty_chosen_char = true;
    }
    ok = ok && rejected_empty_chosen_char;

    bool rejected_zero_candidate = false;
    try {
        ime::fcitx5::PredictResponse invalid_response;
        invalid_response.candidates.push_back({0});
        (void)ime::fcitx5::encode_message(invalid_response);
    } catch (...) {
        rejected_zero_candidate = true;
    }
    ok = ok && rejected_zero_candidate;

    bool rejected_decoded_zero_candidate = false;
    try {
        auto invalid_response_bytes = response_bytes;
        for (size_t i = 0; i < sizeof(char32_t); ++i) invalid_response_bytes[invalid_response_bytes.size() - 1 - i] = 0;
        (void)ime::fcitx5::decode_predict_response(invalid_response_bytes);
    } catch (...) {
        rejected_decoded_zero_candidate = true;
    }
    ok = ok && rejected_decoded_zero_candidate;

    bool rejected_wrong_type = false;
    try {
        (void)ime::fcitx5::decode_status_response(bytes);
    } catch (...) {
        rejected_wrong_type = true;
    }
    ok = ok && rejected_wrong_type;

    bool rejected_missing_field = false;
    try {
        auto truncated = bytes;
        truncated.pop_back();
        (void)ime::fcitx5::decode_predict_request(truncated);
    } catch (...) {
        rejected_missing_field = true;
    }
    ok = ok && rejected_missing_field;

    bool rejected_invalid_scalar = false;
    try {
        (void)ime::fcitx5::char32_to_utf8(0xD800);
    } catch (...) {
        rejected_invalid_scalar = true;
    }
    ok = ok && rejected_invalid_scalar;

    const auto socket_path = std::filesystem::temp_directory_path() / "llavon-ime-protocol-test.sock";
    ime::fcitx5::UnixSocketServer server;
    server.bind_listen(socket_path);
    std::thread server_thread([&server]() {
        auto accepted = server.accept_one();
        const auto received = accepted.recv_exact(3);
        accepted.send_all(received);
    });
    auto client = ime::fcitx5::UnixSocketClient{}.connect(socket_path);
    const std::vector<std::uint8_t> payload{1, 2, 3};
    client.send_all(payload);
    ok = ok && client.recv_exact(3) == payload;
    server_thread.join();

    const auto active_socket_path = std::filesystem::temp_directory_path() / "llavon-ime-active-test.sock";
    ime::fcitx5::UnixSocketServer active_server;
    active_server.bind_listen(active_socket_path);
    bool rejected_active_socket = false;
    try {
        ime::fcitx5::UnixSocketServer second_server;
        second_server.bind_listen(active_socket_path);
    } catch (...) {
        rejected_active_socket = true;
    }
    ok = ok && rejected_active_socket;

    const auto parentless_socket = std::filesystem::path("llavon-ime-parentless-test.sock");
    {
        ime::fcitx5::UnixSocketServer parentless_server;
        parentless_server.bind_listen(parentless_socket);
    }
    ok = ok && !std::filesystem::exists(parentless_socket);

    const auto non_socket_path = std::filesystem::temp_directory_path() / "llavon-ime-non-socket-test";
    {
        std::ofstream file(non_socket_path);
        file << "not a socket";
    }
    bool rejected_non_socket = false;
    try {
        ime::fcitx5::UnixSocketServer non_socket_server;
        non_socket_server.bind_listen(non_socket_path);
    } catch (...) {
        rejected_non_socket = true;
    }
    ok = ok && rejected_non_socket;
    ok = ok && std::filesystem::exists(non_socket_path);
    std::filesystem::remove(non_socket_path);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
