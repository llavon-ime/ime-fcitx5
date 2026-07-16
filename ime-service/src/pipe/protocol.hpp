#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace imesvc::protocol {

inline constexpr std::uint32_t kMaxFramePayloadBytes = 1024U * 1024U;
inline constexpr std::uint32_t kMaxRepeatedFields = 65536U;
inline constexpr std::uint32_t kMaxContextCodeUnits = 65536U;
inline constexpr std::uint32_t kMaxBopomofoCodeUnits = 4096U;
inline constexpr std::uint32_t kMaxUtf8StringBytes = 256U * 1024U;
inline constexpr char16_t kBopomofoReadingSeparator = u'\x1f';

using ByteVector = std::vector<std::uint8_t>;
using SessionId = std::array<std::uint8_t, 16>;
using ServiceEpoch = std::array<std::uint8_t, 16>;
using EventId = std::array<std::uint8_t, 16>;

using ProtocolVersion = std::uint16_t;
inline constexpr ProtocolVersion kProtocolVersion = 3;
inline constexpr ProtocolVersion kMinProtocolVersion = kProtocolVersion;
inline constexpr ProtocolVersion kMaxProtocolVersion = kProtocolVersion;

enum class Capability : std::uint64_t {
    PersonalFeedback = 1ULL << 0U,
    TrainingStatus = 1ULL << 1U,
    DeletePersonalData = 1ULL << 2U,
};

using Capabilities = std::uint64_t;
inline constexpr Capabilities capability_mask(Capability capability) noexcept {
    return static_cast<Capabilities>(capability);
}
inline constexpr Capabilities kSupportedCapabilities =
    capability_mask(Capability::PersonalFeedback) | capability_mask(Capability::TrainingStatus) |
    capability_mask(Capability::DeletePersonalData);
inline constexpr bool has_capability(Capabilities capabilities, Capability capability) noexcept {
    return (capabilities & capability_mask(capability)) != 0;
}

enum class MessageType : std::uint8_t {
    OpenSession = 1,
    Predict = 2,
    Prediction = 3,
    CloseSession = 4,
    Status = 5,
    Shutdown = 6,
    Error = 7,
    Hello = 8,
    Feedback = 9,
    TrainingStatus = 10,
    DeletePersonalData = 11,
};

enum class ErrorCode : std::uint8_t {
    UnknownSession = 1,
    InvalidArgument = 2,
    ProtocolError = 3,
    Unauthorized = 4,
    SessionBusy = 5,
    OutOfOrder = 6,
    ResourceExhausted = 7,
    ModelError = 8,
    ServiceShuttingDown = 9,
};

struct PaddingEntry {
    bool chosen = false;
    std::u16string bopomofo;
    char32_t chosen_char = 0;
};

struct HelloRequest {
    ProtocolVersion minimum_version = kMinProtocolVersion;
    ProtocolVersion maximum_version = kMaxProtocolVersion;
    Capabilities capabilities = 0;
};

struct HelloResponse {
    ProtocolVersion version = 0;
    Capabilities capabilities = 0;
};

struct OpenSessionRequest {};
struct OpenSessionResponse {
    SessionId session_id{};
    ServiceEpoch service_epoch{};
};

struct PredictRequest {
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::u16string context;
    std::vector<PaddingEntry> padding;
};

struct Prediction {
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::vector<std::vector<char32_t>> candidates;
    EventId feedback_token{};
};

struct CloseSessionRequest {
    SessionId session_id{};
};
struct CloseSessionResponse {
    SessionId session_id{};
    bool closed = false;
};

struct StatusRequest {
    std::optional<SessionId> session_id;
};

struct SessionStatus {
    SessionId session_id{};
    bool active = false;
    bool closing = false;
    bool engine_loaded = false;
    std::uint64_t last_request_id = 0;
};

struct StatusResponse {
    ServiceEpoch service_epoch{};
    bool shutting_down = false;
    bool model_loaded = false;
    bool tables_loaded = false;
    bool inference_model_loaded = false;
    bool adapter_loaded = false;
    std::string active_adapter_version;
    std::string model_error;
    std::uint32_t active_sessions = 0;
    std::uint32_t max_sessions = 0;
    std::optional<SessionStatus> session;
};

struct ShutdownRequest {};
struct ShutdownResponse {
    bool accepted = false;
};

enum class FeedbackSignal : std::uint8_t {
    ExplicitCorrection = 1,
    ExplicitTop1Selection = 2,
    AcceptedPrediction = 3,
    FallbackCommit = 4,
};

struct FeedbackRequest {
    EventId event_id{};
    std::u16string left_context;
    std::u16string bopomofo_sequence;
    std::u16string committed_characters;
    std::u16string predicted_top1;
    std::vector<bool> manually_chosen_flags;
    FeedbackSignal signal_type = FeedbackSignal::FallbackCommit;
    std::string base_model_hash;
    std::string adapter_version;
    std::uint64_t created_at = 0;
    SessionId session_id{};
    EventId feedback_token{};
};

struct FeedbackAccepted {
    EventId event_id{};
};

struct TrainingStatusRequest {};

struct TrainingStatusResponse {
    bool collecting = false;
    bool training = false;
    std::uint64_t accepted_feedback_count = 0;
    std::uint64_t eligible_character_count = 0;
    std::string active_adapter_version;
    std::string state;
    std::string message;
};

struct DeletePersonalDataRequest {};
struct DeletePersonalDataResponse {
    bool deleted = false;
};

struct Error {
    ErrorCode code = ErrorCode::ProtocolError;
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::string message;
};

using Message = std::variant<HelloRequest, HelloResponse, OpenSessionRequest, OpenSessionResponse,
                             PredictRequest, Prediction, CloseSessionRequest, CloseSessionResponse,
                             StatusRequest, StatusResponse, ShutdownRequest, ShutdownResponse,
                              FeedbackRequest, FeedbackAccepted, TrainingStatusRequest,
                              TrainingStatusResponse, DeletePersonalDataRequest, DeletePersonalDataResponse, Error>;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};

ByteVector encode(const Message& message);
Message decode(const ByteVector& frame);
MessageType message_type(const Message& message);
MessageType decode_message_type(const ByteVector& frame);
const char* error_code_name(ErrorCode code) noexcept;

bool valid_scalar(char32_t value) noexcept;
bool valid_utf16(const std::u16string& value) noexcept;
bool is_zero(const SessionId& id) noexcept;

}  // namespace imesvc::protocol
