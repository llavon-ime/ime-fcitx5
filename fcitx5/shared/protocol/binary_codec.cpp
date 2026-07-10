#include "protocol/binary_codec.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ime::fcitx5 {

namespace {

constexpr std::uint32_t kMaxFramePayloadBytes = 1024 * 1024;
constexpr std::uint32_t kMaxRepeatedFields = 65536;

enum class MessageType : std::uint8_t {
    PredictRequest = 1,
    PredictResponse = 2,
    StatusResponse = 3,
    ControlRequest = 4,
};

enum class ControlOperation : std::uint8_t {
    Status = 1,
    Stop = 2,
    ReloadConfig = 3,
};

template <typename T>
void append_raw(ByteVector& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

void append_bytes(ByteVector& bytes, const void* data, size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), begin, begin + size);
}

std::uint32_t checked_u32_size(size_t size, const char* field) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("protocol field is too large: ") + field);
    }
    return static_cast<std::uint32_t>(size);
}

void append_string(ByteVector& bytes, const std::string& value) {
    const auto size = checked_u32_size(value.size(), "string");
    append_raw(bytes, size);
    append_bytes(bytes, value.data(), value.size());
}

void append_u16string(ByteVector& bytes, const std::u16string& value) {
    const auto size = checked_u32_size(value.size(), "u16string");
    append_raw(bytes, size);
    append_bytes(bytes, value.data(), value.size() * sizeof(char16_t));
}

ByteVector make_frame(ByteVector payload) {
    if (payload.size() > kMaxFramePayloadBytes) throw std::runtime_error("protocol payload too large");

    ByteVector bytes;
    bytes.reserve(sizeof(std::uint32_t) + payload.size());
    const auto length = static_cast<std::uint32_t>(payload.size());
    append_raw(bytes, length);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

class Reader {
public:
    explicit Reader(const ByteVector& bytes) : bytes_(bytes) {
        const auto length = read_raw<std::uint32_t>();
        if (length > kMaxFramePayloadBytes) throw std::runtime_error("protocol frame is too large");
        if (bytes_.size() != sizeof(std::uint32_t) + static_cast<size_t>(length)) {
            throw std::runtime_error("protocol message length mismatch");
        }
    }

    MessageType read_message_type() {
        const auto raw = read_raw<std::uint8_t>();
        switch (static_cast<MessageType>(raw)) {
            case MessageType::PredictRequest:
            case MessageType::PredictResponse:
            case MessageType::StatusResponse:
            case MessageType::ControlRequest:
                return static_cast<MessageType>(raw);
            default:
                throw std::runtime_error("unknown protocol message type: " + std::to_string(raw));
        }
    }

    template <typename T>
    T read_raw() {
        static_assert(std::is_trivially_copyable_v<T>);
        require(sizeof(T));
        T value{};
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    std::string read_string(const char* field) {
        const auto size = read_raw<std::uint32_t>();
        if (size > kMaxFramePayloadBytes) throw std::runtime_error(std::string("protocol string is too large: ") + field);
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return value;
    }

    std::u16string read_u16string(const char* field) {
        const auto length = read_raw<std::uint32_t>();
        if (length > kMaxFramePayloadBytes / sizeof(char16_t)) {
            throw std::runtime_error(std::string("protocol u16string is too large: ") + field);
        }
        const size_t byte_count = static_cast<size_t>(length) * sizeof(char16_t);
        require(byte_count);
        std::u16string value(length, u'\0');
        if (byte_count > 0) std::memcpy(value.data(), bytes_.data() + offset_, byte_count);
        offset_ += byte_count;
        return value;
    }

    void require_done() const {
        if (offset_ != bytes_.size()) throw std::runtime_error("protocol message has trailing bytes");
    }

private:
    void require(size_t size) const {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_) {
            throw std::runtime_error("protocol message ended unexpectedly");
        }
    }

    const ByteVector& bytes_;
    size_t offset_ = 0;
};

void require_type(MessageType actual, MessageType expected) {
    if (actual != expected) throw std::runtime_error("unexpected protocol message type");
}

ControlOperation encode_operation(std::string_view operation) {
    if (operation == "status") return ControlOperation::Status;
    if (operation == "stop") return ControlOperation::Stop;
    if (operation == "reload_config") return ControlOperation::ReloadConfig;
    throw std::runtime_error("unknown control operation: " + std::string(operation));
}

std::string decode_operation(ControlOperation operation) {
    switch (operation) {
        case ControlOperation::Status:
            return "status";
        case ControlOperation::Stop:
            return "stop";
        case ControlOperation::ReloadConfig:
            return "reload_config";
        default:
            throw std::runtime_error("unknown control operation");
    }
}

void append_message_type(ByteVector& payload, MessageType type) {
    append_raw(payload, static_cast<std::uint8_t>(type));
}

}  // namespace

ByteVector encode_message(const PredictRequest& request) {
    ByteVector payload;
    append_message_type(payload, MessageType::PredictRequest);
    append_u16string(payload, request.context);

    const auto padding_count = checked_u32_size(request.padding.size(), "padding");
    append_raw(payload, padding_count);
    for (const auto& entry : request.padding) {
        const auto kind = static_cast<std::uint8_t>(entry.chosen ? 1 : 0);
        append_raw(payload, kind);
        if (entry.chosen) {
            if (entry.chosen_char == 0) throw std::runtime_error("chosen padding requires chosen_char");
            append_raw(payload, entry.chosen_char);
        } else {
            append_u16string(payload, entry.bopomofo);
        }
    }

    return make_frame(std::move(payload));
}

ByteVector encode_message(const PredictResponse& response) {
    ByteVector payload;
    append_message_type(payload, MessageType::PredictResponse);

    const auto segment_count = checked_u32_size(response.candidates.size(), "candidates");
    append_raw(payload, segment_count);
    for (const auto& list : response.candidates) {
        const auto candidate_count = checked_u32_size(list.size(), "candidate list");
        append_raw(payload, candidate_count);
        for (char32_t candidate : list) {
            if (candidate == 0) throw std::runtime_error("candidate must contain a nonzero codepoint");
            append_raw(payload, candidate);
        }
    }

    return make_frame(std::move(payload));
}

ByteVector encode_message(const StatusResponse& response) {
    ByteVector payload;
    append_message_type(payload, MessageType::StatusResponse);
    append_raw(payload, static_cast<std::uint8_t>(response.running ? 1 : 0));
    append_raw(payload, static_cast<std::uint8_t>(response.model_loaded ? 1 : 0));
    append_string(payload, response.backend);
    append_string(payload, response.model_path);
    append_string(payload, response.error);
    return make_frame(std::move(payload));
}

ByteVector encode_message(const ControlRequest& request) {
    ByteVector payload;
    append_message_type(payload, MessageType::ControlRequest);
    append_raw(payload, static_cast<std::uint8_t>(encode_operation(request.operation)));
    return make_frame(std::move(payload));
}

PredictRequest decode_predict_request(const ByteVector& bytes) {
    Reader reader(bytes);
    require_type(reader.read_message_type(), MessageType::PredictRequest);

    PredictRequest request;
    request.context = reader.read_u16string("context");

    const auto padding_count = reader.read_raw<std::uint32_t>();
    if (padding_count > kMaxRepeatedFields) throw std::runtime_error("too many protocol padding entries");
    request.padding.reserve(padding_count);
    for (std::uint32_t i = 0; i < padding_count; ++i) {
        PaddingEntry entry;
        const auto kind = reader.read_raw<std::uint8_t>();
        if (kind == 0) {
            entry.bopomofo = reader.read_u16string("bopomofo");
        } else if (kind == 1) {
            entry.chosen = true;
            entry.chosen_char = reader.read_raw<char32_t>();
            if (entry.chosen_char == 0) throw std::runtime_error("chosen padding requires chosen_char");
        } else {
            throw std::runtime_error("unknown padding entry type");
        }
        request.padding.push_back(std::move(entry));
    }

    reader.require_done();
    return request;
}

PredictResponse decode_predict_response(const ByteVector& bytes) {
    Reader reader(bytes);
    require_type(reader.read_message_type(), MessageType::PredictResponse);

    PredictResponse response;
    const auto segment_count = reader.read_raw<std::uint32_t>();
    if (segment_count > kMaxRepeatedFields) throw std::runtime_error("too many protocol candidate lists");
    response.candidates.reserve(segment_count);
    for (std::uint32_t i = 0; i < segment_count; ++i) {
        const auto candidate_count = reader.read_raw<std::uint32_t>();
        if (candidate_count > kMaxRepeatedFields) throw std::runtime_error("too many protocol candidates");
        std::vector<char32_t> list;
        list.reserve(candidate_count);
        for (std::uint32_t j = 0; j < candidate_count; ++j) {
            const auto candidate = reader.read_raw<char32_t>();
            if (candidate == 0) throw std::runtime_error("candidate must contain a nonzero codepoint");
            list.push_back(candidate);
        }
        response.candidates.push_back(std::move(list));
    }

    reader.require_done();
    return response;
}

StatusResponse decode_status_response(const ByteVector& bytes) {
    Reader reader(bytes);
    require_type(reader.read_message_type(), MessageType::StatusResponse);

    StatusResponse response;
    response.running = reader.read_raw<std::uint8_t>() != 0;
    response.model_loaded = reader.read_raw<std::uint8_t>() != 0;
    response.backend = reader.read_string("backend");
    response.model_path = reader.read_string("model_path");
    response.error = reader.read_string("error");
    reader.require_done();
    return response;
}

ControlRequest decode_control_request(const ByteVector& bytes) {
    Reader reader(bytes);
    require_type(reader.read_message_type(), MessageType::ControlRequest);

    const auto raw_operation = reader.read_raw<std::uint8_t>();
    ControlRequest request;
    request.operation = decode_operation(static_cast<ControlOperation>(raw_operation));
    reader.require_done();
    return request;
}

std::string decode_message_type(const ByteVector& bytes) {
    Reader reader(bytes);
    switch (reader.read_message_type()) {
        case MessageType::PredictRequest:
            return "predict_request";
        case MessageType::PredictResponse:
            return "predict_response";
        case MessageType::StatusResponse:
            return "status_response";
        case MessageType::ControlRequest:
            return "control_request";
        default:
            throw std::runtime_error("unknown protocol message type");
    }
}

}  // namespace ime::fcitx5
