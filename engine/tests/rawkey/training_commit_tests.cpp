#include "raw_key_harness.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("training commit samples", training_commit_samples) {
    struct Observed {
        llavon::ime::InputEffect::CommitSample sample;
        std::u16string context;
    };
    std::vector<Observed> samples;
    HarnessOptions options;
    options.config.collect_training_data = true;
    options.on_training_commit = [&](const auto& sample, std::u16string_view context) {
        samples.push_back({sample, std::u16string(context)});
    };
    Harness harness(options);
    harness.set_surrounding("早安", 2, 2);

    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(samples.size() == 1);
    RAWKEY_ASSERT(samples[0].sample.answer == u"你");
    RAWKEY_ASSERT(samples[0].sample.entries.size() == 1);
    RAWKEY_ASSERT(samples[0].sample.entries[0].reading == u"ㄋㄧˇ");
    RAWKEY_ASSERT(samples[0].sample.entries[0].character == U'你');
    RAWKEY_ASSERT(!samples[0].sample.entries[0].manually_selected);
    RAWKEY_ASSERT(samples[0].context == u"早安");

    harness.type("su3");
    harness.key("space");
    harness.key("2");
    if (samples.size() == 1) harness.key("Return");
    RAWKEY_ASSERT(samples.size() == 2);
    RAWKEY_ASSERT(samples[1].sample.entries[0].manually_selected);

    harness.host().set_sensitive(true);
    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(samples.size() == 2);

    harness.host().set_sensitive(false);
    harness.type("abc");
    harness.key("Return");
    RAWKEY_ASSERT(samples.size() == 2);

    harness.set_config("CollectTrainingData", "False");
    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(samples.size() == 2);
}

RAWKEY_SUITE("training commit transport", training_commit_transport) {
    using namespace llavon::ime;
    const auto socket = std::filesystem::temp_directory_path() /
                        ("llavon-ime-rawkey-commit-" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket);
    UnixSocketServer server;
    server.bind_listen(socket);
    std::atomic<bool> received{false};
    std::atomic<bool> valid{true};
    std::thread worker([&] {
        try {
            const auto connection = server.accept_one();
            protocol::SessionId session{}; session[0] = 1;
            protocol::ServiceEpoch epoch{}; epoch[0] = 2;
            for (;;) {
                auto header = connection.recv_exact(4);
                std::uint32_t length = 0;
                std::memcpy(&length, header.data(), 4);
                auto payload = connection.recv_exact(length);
                header.insert(header.end(), payload.begin(), payload.end());
                const auto message = protocol::decode(header);
                if (std::holds_alternative<protocol::StatusRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}));
                } else if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::OpenSessionResponse{session, epoch}));
                } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Prediction{
                        session, request->request_id, request->buffer_revision, {{U'你'}}}));
                } else if (const auto* request = std::get_if<protocol::RecordCommitRequest>(&message)) {
                    valid = request->context == u"早安" && request->answer == u"你" &&
                            request->entries.size() == 1 && request->entries[0].reading == u"ㄋㄧˇ";
                    connection.send_all(protocol::encode(protocol::RecordCommitResponse{request->event_id, true}));
                    received = true;
                    break;
                } else if (const auto* request = std::get_if<protocol::CloseSessionRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::CloseSessionResponse{request->session_id, true}));
                    break;
                } else { valid = false; break; }
            }
        } catch (...) { valid = false; }
    });
    bool completed = false;
    {
        HarnessOptions options;
        options.socket_path = socket.string();
        options.config.collect_training_data = true;
        Harness harness(options);
        harness.set_surrounding("早安", 2, 2);
        harness.type("su3");
        harness.expect_commit("你");
        completed = harness.pump_until([&] { return received.load(); });
        harness.detach();
    }
    if (!received.load()) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket);
        } catch (...) {}
    }
    worker.join();
    std::filesystem::remove(socket);
    RAWKEY_ASSERT(completed && valid.load());
}
