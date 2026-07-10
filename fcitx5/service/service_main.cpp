#include <unistd.h>

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include "config/config.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/binary_codec.hpp"
#include "service/service_app.hpp"
#include "text/utf.hpp"

namespace {

using LocalSocket = asio::local::stream_protocol;
using ResponseMessage = std::variant<ime::fcitx5::PredictResponse, ime::fcitx5::StatusResponse>;

constexpr std::uint32_t kMaxFramePayloadBytes = 1024 * 1024;

nlohmann::json status_json(const ime::fcitx5::StatusResponse& status) {
    return nlohmann::json{
        {"running", status.running},
        {"model_loaded", status.model_loaded},
        {"backend", status.backend},
        {"model_path", status.model_path},
        {"error", status.error}};
}

std::uint32_t frame_length(const std::array<std::uint8_t, 4>& header) {
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    return length;
}

ime::fcitx5::ByteVector make_frame(const std::array<std::uint8_t, 4>& header,
                                   const ime::fcitx5::ByteVector& payload) {
    ime::fcitx5::ByteVector frame;
    frame.reserve(header.size() + payload.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

ime::fcitx5::ByteVector encode_response(const ResponseMessage& response) {
    return std::visit([](const auto& value) { return ime::fcitx5::encode_message(value); }, response);
}

ime::fcitx5::StatusResponse with_error(ime::fcitx5::StatusResponse status, const std::string& error) {
    status.error = error;
    return status;
}

bool socket_accepts_connections(const std::filesystem::path& path) {
    try {
        auto connection = ime::fcitx5::UnixSocketClient{}.connect(path);
        return connection.valid();
    } catch (const std::system_error& error) {
        const int value = error.code().value();
        if (value == ECONNREFUSED || value == ENOENT) return false;
        throw;
    }
}

void prepare_socket_path(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    if (!std::filesystem::exists(path)) return;

    if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::socket) {
        throw std::runtime_error("refusing to replace non-socket path: " + path.string());
    }
    if (socket_accepts_connections(path)) {
        throw std::runtime_error("Unix socket is already in use: " + path.string());
    }
    std::filesystem::remove(path);
}

class SocketPathCleanup {
public:
    SocketPathCleanup() = default;
    explicit SocketPathCleanup(std::filesystem::path path) : path_(std::move(path)), armed_(true) {}

    ~SocketPathCleanup() {
        if (!armed_ || path_.empty()) return;

        std::error_code ec;
        if (std::filesystem::symlink_status(path_, ec).type() == std::filesystem::file_type::socket) {
            std::filesystem::remove(path_, ec);
        }
    }

    SocketPathCleanup(const SocketPathCleanup&) = delete;
    SocketPathCleanup& operator=(const SocketPathCleanup&) = delete;

    void arm(std::filesystem::path path) {
        path_ = std::move(path);
        armed_ = true;
    }

private:
    std::filesystem::path path_;
    bool armed_ = false;
};

class PidFile {
public:
    explicit PidFile(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream pid(path_);
        if (!pid) {
            std::filesystem::remove(path_);
            throw std::runtime_error("failed to open PID file: " + path_.string());
        }
        pid << getpid() << '\n';
        if (!pid) {
            std::filesystem::remove(path_);
            throw std::runtime_error("failed to write PID file: " + path_.string());
        }
    }

    ~PidFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

private:
    std::filesystem::path path_;
};

class InferenceWorker {
public:
    using Callback = std::function<void(ResponseMessage)>;

    InferenceWorker(ime::fcitx5::Config config, int idle_timeout_seconds)
        : app_(std::move(config)),
          idle_timeout_seconds_(idle_timeout_seconds),
          last_activity_(std::chrono::steady_clock::now()) {
        update_status(app_.handle_status());
        worker_ = std::thread([this]() { run(); });
    }

    ~InferenceWorker() {
        shutdown();
    }

    InferenceWorker(const InferenceWorker&) = delete;
    InferenceWorker& operator=(const InferenceWorker&) = delete;

    void enqueue(ime::fcitx5::PredictRequest request, Callback callback) {
        bool stopped = false;
        {
            std::lock_guard lock(mutex_);
            stopped = stopping_;
            if (!stopped) {
                queue_.push_back(PredictJob{std::move(request), std::move(callback)});
                last_activity_ = std::chrono::steady_clock::now();
            }
        }

        if (stopped) {
            if (callback) callback(stopped_status());
            return;
        }

        cv_.notify_one();
    }

    ime::fcitx5::StatusResponse status() const {
        std::lock_guard lock(status_mutex_);
        return status_;
    }

    ime::fcitx5::StatusResponse request_stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();

        auto status = this->status();
        status.running = false;
        update_status(status);
        return status;
    }

    bool expired_idle_timeout(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        std::lock_guard lock(mutex_);
        if (stopping_ || busy_ || !queue_.empty()) return false;
        return now - last_activity_ >= std::chrono::seconds(idle_timeout_seconds_);
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

private:
    struct PredictJob {
        ime::fcitx5::PredictRequest request;
        Callback callback;
    };

    ResponseMessage stopped_status() const {
        auto status = this->status();
        status.running = false;
        if (status.error.empty()) status.error = "service is stopping";
        return status;
    }

    ResponseMessage handle_predict(const ime::fcitx5::PredictRequest& request) {
        try {
            auto response = app_.handle_predict(request);
            update_status_for_current_state(app_.handle_status());
            return response;
        } catch (const std::exception& error) {
            auto status = app_.handle_status();
            status.error = error.what();
            update_status_for_current_state(status);
            return status;
        }
    }

    void run() {
        while (true) {
            PredictJob job;
            bool has_job = false;
            std::deque<PredictJob> cancelled;

            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (stopping_) {
                    cancelled.swap(queue_);
                } else {
                    job = std::move(queue_.front());
                    queue_.pop_front();
                    busy_ = true;
                    has_job = true;
                }
            }

            for (auto& cancelled_job : cancelled) {
                if (cancelled_job.callback) cancelled_job.callback(stopped_status());
            }
            if (!has_job) break;

            auto response = handle_predict(job.request);
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
                last_activity_ = std::chrono::steady_clock::now();
            }
            if (job.callback) job.callback(std::move(response));
        }

        app_.handle_stop();
        update_status(app_.handle_status());
    }

    void update_status(ime::fcitx5::StatusResponse status) const {
        std::lock_guard lock(status_mutex_);
        status_ = std::move(status);
    }

    void update_status_for_current_state(ime::fcitx5::StatusResponse status) const {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) status.running = false;
        }
        update_status(std::move(status));
    }

    ime::fcitx5::ServiceApp app_;
    int idle_timeout_seconds_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PredictJob> queue_;
    bool stopping_ = false;
    bool busy_ = false;
    std::chrono::steady_clock::time_point last_activity_;
    std::thread worker_;

    mutable std::mutex status_mutex_;
    mutable ime::fcitx5::StatusResponse status_;
};

class AsyncConnection : public std::enable_shared_from_this<AsyncConnection> {
public:
    AsyncConnection(asio::io_context& io, InferenceWorker& worker, std::function<void()> request_stop)
        : socket_(io), worker_(worker), request_stop_(std::move(request_stop)) {}

    LocalSocket::socket& socket() {
        return socket_;
    }

    void start() {
        read_header();
    }

private:
    void read_header() {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(header_), [self](const asio::error_code& error, size_t) {
            if (error) return;

            const auto length = frame_length(self->header_);
            if (length > kMaxFramePayloadBytes) {
                self->write_status_error("protocol frame is too large");
                return;
            }

            self->payload_.assign(length, 0);
            self->read_payload();
        });
    }

    void read_payload() {
        if (payload_.empty()) {
            handle_frame();
            return;
        }

        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(payload_), [self](const asio::error_code& error, size_t) {
            if (error) return;
            self->handle_frame();
        });
    }

    void handle_frame() {
        frame_ = make_frame(header_, payload_);
        try {
            const auto type = ime::fcitx5::decode_message_type(frame_);
            if (type == "control_request") {
                handle_control_request();
                return;
            }
            if (type == "predict_request") {
                handle_predict_request();
                return;
            }

            write_status_error("unsupported client message type: " + type);
        } catch (const std::exception& error) {
            write_status_error(error.what());
        }
    }

    void handle_control_request() {
        const auto request = ime::fcitx5::decode_control_request(frame_);
        if (request.operation == "stop") {
            auto status = worker_.request_stop();
            write_message(ime::fcitx5::encode_message(status), request_stop_);
            return;
        }

        write_message(ime::fcitx5::encode_message(worker_.status()));
    }

    void handle_predict_request() {
        auto request = ime::fcitx5::decode_predict_request(frame_);
        auto self = shared_from_this();
        worker_.enqueue(std::move(request), [self](ResponseMessage response) mutable {
            asio::post(self->socket_.get_executor(), [self, response = std::move(response)]() mutable {
                self->write_response(std::move(response));
            });
        });
    }

    void write_response(ResponseMessage response) {
        try {
            write_message(encode_response(response));
        } catch (const std::exception& error) {
            write_status_error(error.what());
        }
    }

    void write_status_error(const std::string& error) {
        try {
            write_message(ime::fcitx5::encode_message(with_error(worker_.status(), error)));
        } catch (...) {
            close();
        }
    }

    void write_message(ime::fcitx5::ByteVector bytes, std::function<void()> on_complete = {}) {
        write_buffer_ = std::move(bytes);
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(write_buffer_),
                          [self, on_complete = std::move(on_complete)](const asio::error_code&, size_t) mutable {
                              self->close();
                              if (on_complete) on_complete();
                          });
    }

    void close() {
        asio::error_code ignored;
        socket_.shutdown(LocalSocket::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    LocalSocket::socket socket_;
    InferenceWorker& worker_;
    std::function<void()> request_stop_;
    std::array<std::uint8_t, 4> header_{};
    ime::fcitx5::ByteVector payload_;
    ime::fcitx5::ByteVector frame_;
    ime::fcitx5::ByteVector write_buffer_;
};

class AsyncService {
public:
    AsyncService(asio::io_context& io, InferenceWorker& worker, std::filesystem::path socket_path)
        : io_(io), acceptor_(io), worker_(worker), idle_timer_(io), socket_path_(std::move(socket_path)) {
        prepare_socket_path(socket_path_);
        acceptor_.open(LocalSocket());
        acceptor_.bind(LocalSocket::endpoint(socket_path_.string()));
        acceptor_.listen(16);
        cleanup_.arm(socket_path_);
    }

    void start() {
        accept_next();
        schedule_idle_check();
    }

    void stop() {
        if (stopped_) return;
        stopped_ = true;

        asio::error_code ignored;
        acceptor_.close(ignored);
        idle_timer_.cancel(ignored);
    }

private:
    void accept_next() {
        auto connection = std::make_shared<AsyncConnection>(io_, worker_, [this]() { stop(); });
        acceptor_.async_accept(connection->socket(), [this, connection](const asio::error_code& error) {
            if (!error) connection->start();
            if (!stopped_) accept_next();
        });
    }

    void schedule_idle_check() {
        idle_timer_.expires_after(std::chrono::seconds(1));
        idle_timer_.async_wait([this](const asio::error_code& error) {
            if (error || stopped_) return;
            if (worker_.expired_idle_timeout()) {
                worker_.request_stop();
                stop();
                return;
            }
            schedule_idle_check();
        });
    }

    asio::io_context& io_;
    LocalSocket::acceptor acceptor_;
    InferenceWorker& worker_;
    asio::steady_timer idle_timer_;
    std::filesystem::path socket_path_;
    SocketPathCleanup cleanup_;
    bool stopped_ = false;
};

int print_status(ime::fcitx5::ServiceApp& app) {
    std::cout << status_json(app.handle_status()).dump() << '\n';
    return EXIT_SUCCESS;
}

int smoke_predict(ime::fcitx5::ServiceApp& app, const std::string& bopomofo) {
    ime::fcitx5::PredictRequest request;
    request.padding.push_back({false, ime::fcitx5::utf8_to_u16(bopomofo), 0});

    const auto response = app.handle_predict(request);
    if (response.candidates.empty() || response.candidates.front().empty()) return EXIT_FAILURE;

    for (size_t i = 0; i < response.candidates.front().size(); ++i) {
        if (i != 0) std::cout << ' ';
        std::cout << ime::fcitx5::char32_to_utf8(response.candidates.front()[i]);
    }
    std::cout << '\n';
    return EXIT_SUCCESS;
}

int run(int argc, char** argv) {
    auto config = ime::fcitx5::load_config();
    if (const char* model_path = std::getenv("IME_FCITX5_MODEL_PATH")) {
        config.model_path = model_path;
    }

    if (argc >= 2 && std::string(argv[1]) == "--status") {
        ime::fcitx5::ServiceApp app(config);
        return print_status(app);
    }
    if (argc >= 2 && std::string(argv[1]) == "--smoke-predict") {
        if (argc < 3) throw std::runtime_error("--smoke-predict requires a bopomofo syllable");
        ime::fcitx5::ServiceApp app(config);
        return smoke_predict(app, argv[2]);
    }

    asio::io_context io;
    InferenceWorker worker(config, config.idle_timeout_seconds);
    AsyncService service(io, worker, ime::fcitx5::socket_path());
    PidFile pid_file(ime::fcitx5::pid_path());
    service.start();
    io.run();
    worker.shutdown();

    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
