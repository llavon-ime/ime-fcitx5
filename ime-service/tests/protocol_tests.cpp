#include "engine/model_runtime.hpp"
#include "pipe/protocol.hpp"
#include "session/session_manager.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {

bool protocol_test() {
    using namespace imesvc::protocol;
    OpenSessionResponse opened;
    for (std::size_t i = 0; i < opened.session_id.size(); ++i) {
        opened.session_id[i] = static_cast<std::uint8_t>(i + 1);
        opened.service_epoch[i] = static_cast<std::uint8_t>(0xf0U - i);
    }
    const auto bytes = encode(Message{opened});
    const ByteVector expected{34, 0, 0, 0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                              0xf0, 0xef, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4,
                              0xe3, 0xe2, 0xe1};
    if (bytes != expected) return false;
    auto decoded = decode(bytes);
    if (std::get<OpenSessionResponse>(decoded).service_epoch != opened.service_epoch) return false;
    FeedbackRequest feedback;
    feedback.event_id[0] = 1;
    feedback.bopomofo_sequence = u"ㄋㄧˇ";
    feedback.committed_characters = u"你";
    feedback.predicted_top1 = u"你";
    feedback.manually_chosen_flags = {false};
    feedback.signal_type = FeedbackSignal::AcceptedPrediction;
    feedback.base_model_hash = "abc";
    feedback.session_id = opened.session_id;
    feedback.feedback_token[0] = 1;
    const auto round_trip = decode(encode(Message{feedback}));
    if (std::get<FeedbackRequest>(round_trip).event_id != feedback.event_id) return false;
    auto first_tone_feedback = feedback;
    first_tone_feedback.bopomofo_sequence = u"ㄅ ";
    if (std::get<FeedbackRequest>(decode(encode(Message{first_tone_feedback}))).bopomofo_sequence != u"ㄅ ") return false;
    feedback.bopomofo_sequence = std::u16string(u"ㄋㄧˇ") + kBopomofoReadingSeparator + u"ㄏㄠˇ";
    try {
        (void)encode(Message{feedback});
        return false;
    } catch (const ProtocolError&) {
    }
    feedback.bopomofo_sequence = u"ㄋㄧˇ";
    feedback.predicted_top1.clear();
    try {
        (void)encode(Message{feedback});
        return false;
    } catch (const ProtocolError&) {
    }
    TrainingStatusResponse training_status;
    training_status.collecting = true;
    training_status.training = true;
    training_status.accepted_feedback_count = 5;
    training_status.eligible_character_count = 9;
    training_status.active_adapter_version = "lora-run";
    training_status.state = "running";
    training_status.message = "trainer process is running";
    const auto status_round_trip = decode(encode(Message{training_status}));
    const auto* parsed_status = std::get_if<TrainingStatusResponse>(&status_round_trip);
    if (parsed_status == nullptr || !parsed_status->training || parsed_status->active_adapter_version != "lora-run" ||
        parsed_status->state != "running") return false;
    const auto deletion_round_trip = decode(encode(Message{DeletePersonalDataResponse{true}}));
    if (!std::get<DeletePersonalDataResponse>(deletion_round_trip).deleted) return false;
    return true;
}

class MockEngine final : public imesvc::ISessionEngine {
public:
    std::vector<std::vector<char32_t>> predict(const imesvc::protocol::PredictRequest& request) override {
        std::vector<std::vector<char32_t>> result;
        for (const auto& entry : request.padding) result.push_back(entry.chosen ? std::vector<char32_t>{entry.chosen_char} : std::vector<char32_t>{U'你', U'妳'});
        return result;
    }
    bool loaded() const noexcept override { return true; }
    std::string adapter_version() const override { return "adapter-1"; }
};

bool session_test() {
    imesvc::RuntimeConfig config;
    config.tables_dir = ".";
    auto runtime = std::make_shared<imesvc::SharedModelRuntime>(config);
    imesvc::SessionLimits limits;
    limits.max_sessions = 2;
    limits.max_idle_sessions = 2;
    limits.idle_timeout = std::chrono::seconds(60);
    limits.max_concurrent_predictions = 2;
    imesvc::SessionManager manager(runtime, limits, []() { return std::make_unique<MockEngine>(); });

    const auto first = manager.open_session(1000);
    const auto second = manager.open_session(1000);
    if (!std::holds_alternative<imesvc::protocol::OpenSessionResponse>(first) ||
        !std::holds_alternative<imesvc::protocol::OpenSessionResponse>(second)) return false;
    const auto first_id = std::get<imesvc::protocol::OpenSessionResponse>(first).session_id;

    imesvc::protocol::PredictRequest request;
    request.session_id = first_id;
    request.request_id = 1;
    request.buffer_revision = 9;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    const auto prediction = manager.predict(1000, request);
    if (!std::holds_alternative<imesvc::protocol::Prediction>(prediction)) return false;
    const auto& value = std::get<imesvc::protocol::Prediction>(prediction);
    if (value.candidates.size() != 1 || value.candidates.front().empty() || value.candidates.front().front() != U'你') return false;
    auto chosen_request = request;
    chosen_request.request_id = 2;
    chosen_request.buffer_revision = 10;
    chosen_request.padding[0].chosen = true;
    chosen_request.padding[0].chosen_char = U'妳';
    const auto chosen_prediction = manager.predict(1000, chosen_request);
    if (!std::holds_alternative<imesvc::protocol::Prediction>(chosen_prediction)) return false;
    imesvc::protocol::FeedbackRequest correlated_feedback;
    correlated_feedback.session_id = first_id;
    correlated_feedback.feedback_token = std::get<imesvc::protocol::Prediction>(chosen_prediction).feedback_token;
    correlated_feedback.bopomofo_sequence = u"ㄊㄚ";
    correlated_feedback.committed_characters = u"妳";
    correlated_feedback.predicted_top1 = u"你";
    correlated_feedback.manually_chosen_flags = {true};
    correlated_feedback.signal_type = imesvc::protocol::FeedbackSignal::ExplicitCorrection;
    if (manager.consume_feedback_token(1000, correlated_feedback).has_value()) return false;
    correlated_feedback.bopomofo_sequence = u"ㄋㄧˇ";
    const auto adapter = manager.consume_feedback_token(1000, correlated_feedback);
    if (!adapter || *adapter != "adapter-1" || manager.consume_feedback_token(1000, correlated_feedback).has_value()) return false;

    const auto duplicate = manager.predict(1000, chosen_request);
    if (!std::holds_alternative<imesvc::protocol::Error>(duplicate) ||
        std::get<imesvc::protocol::Error>(duplicate).code != imesvc::protocol::ErrorCode::OutOfOrder) return false;
    const auto unauthorized = manager.status(2000, first_id);
    if (!std::holds_alternative<imesvc::protocol::Error>(unauthorized) ||
        std::get<imesvc::protocol::Error>(unauthorized).code != imesvc::protocol::ErrorCode::Unauthorized) return false;

    const auto closed = manager.close_session(1000, first_id);
    if (!std::holds_alternative<imesvc::protocol::CloseSessionResponse>(closed)) return false;
    return manager.session_count() == 1;
}

}  // namespace

int main() {
    return protocol_test() && session_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
