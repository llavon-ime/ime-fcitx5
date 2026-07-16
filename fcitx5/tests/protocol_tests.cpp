#include "protocol/protocol.hpp"

#include <cstdlib>
#include <vector>

namespace {

bool equal(const ime::fcitx5::protocol::ByteVector& actual, std::initializer_list<unsigned> expected) {
    if (actual.size() != expected.size()) return false;
    std::size_t index = 0;
    for (const auto value : expected) {
        if (actual[index++] != value) return false;
    }
    return true;
}

}  // namespace

int run_protocol_tests() {
    using namespace ime::fcitx5::protocol;
    bool ok = true;

    ok = ok && equal(encode(Message{OpenSessionRequest{}}), {2, 0, 0, 0, 1, 0});

    const auto hello = decode(encode(Message{HelloRequest{}}));
    const auto* hello_request = std::get_if<HelloRequest>(&hello);
    ok = ok && hello_request != nullptr && hello_request->minimum_version == kProtocolVersion &&
         hello_request->capabilities == 0;

    PredictRequest request;
    for (std::size_t i = 0; i < request.session_id.size(); ++i) request.session_id[i] = static_cast<std::uint8_t>(i + 1);
    request.request_id = 0x0102030405060708ULL;
    request.buffer_revision = 0x1112131415161718ULL;
    request.context = u"你";
    request.padding.push_back(PaddingEntry{true, u"ㄏㄠˇ", U'好'});
    const auto bytes = encode(Message{request});
    ok = ok && equal(bytes, {58, 0, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                             8, 7, 6, 5, 4, 3, 2, 1, 24, 23, 22, 21, 20, 19, 18, 17, 1, 0, 0, 0,
                             96, 79, 1, 0, 0, 0, 1, 3, 0, 0, 0, 15, 49, 32, 49, 199, 2, 125, 89, 0, 0});
    const auto decoded = decode(bytes);
    const auto* decoded_request = std::get_if<PredictRequest>(&decoded);
    ok = ok && decoded_request != nullptr && decoded_request->request_id == request.request_id &&
         decoded_request->buffer_revision == request.buffer_revision && decoded_request->padding.size() == 1 &&
          decoded_request->padding.front().chosen && decoded_request->padding.front().chosen_char == U'好' &&
          decoded_request->padding.front().bopomofo == u"ㄏㄠˇ";

    auto trailing = bytes;
    trailing.push_back(0);
    bool rejected = false;
    try {
        (void)decode(trailing);
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    TrainingStatusResponse training_status;
    training_status.collecting = true;
    training_status.training = true;
    training_status.accepted_feedback_count = 7;
    training_status.eligible_character_count = 13;
    training_status.active_adapter_version = "lora-run";
    training_status.state = "running";
    training_status.message = "trainer process is running";
    const auto decoded_training_status = decode(encode(Message{training_status}));
    const auto* parsed_training_status = std::get_if<TrainingStatusResponse>(&decoded_training_status);
    ok = ok && parsed_training_status != nullptr && parsed_training_status->collecting && parsed_training_status->training &&
         parsed_training_status->accepted_feedback_count == 7 && parsed_training_status->active_adapter_version == "lora-run" &&
         parsed_training_status->state == "running";

    const auto decoded_delete = decode(encode(Message{DeletePersonalDataResponse{true}}));
    ok = ok && std::get_if<DeletePersonalDataResponse>(&decoded_delete) != nullptr &&
         std::get<DeletePersonalDataResponse>(decoded_delete).deleted;

    FeedbackRequest feedback;
    feedback.event_id[0] = 1;
    feedback.bopomofo_sequence = u"ㄋㄧˇ";
    feedback.committed_characters = u"你";
    feedback.predicted_top1 = u"你";
    feedback.manually_chosen_flags = {false};
    feedback.signal_type = FeedbackSignal::AcceptedPrediction;
    feedback.base_model_hash = "abc";
    feedback.session_id[0] = 9;
    feedback.feedback_token[0] = 1;
    feedback.created_at = 1;
    const auto decoded_feedback = decode(encode(Message{feedback}));
    const auto* decoded_feedback_request = std::get_if<FeedbackRequest>(&decoded_feedback);
    ok = ok && decoded_feedback_request != nullptr && decoded_feedback_request->event_id == feedback.event_id &&
         decoded_feedback_request->committed_characters == feedback.committed_characters;
    auto first_tone_feedback = feedback;
    first_tone_feedback.bopomofo_sequence = u"ㄅ ";
    const auto decoded_first_tone = decode(encode(Message{first_tone_feedback}));
    ok = ok && std::get<FeedbackRequest>(decoded_first_tone).bopomofo_sequence == u"ㄅ ";

    auto invalid_feedback = feedback;
    invalid_feedback.bopomofo_sequence = std::u16string(u"ㄋㄧˇ") + kBopomofoReadingSeparator + u"ㄏㄠˇ";
    rejected = false;
    try {
        (void)encode(Message{invalid_feedback});
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    invalid_feedback = feedback;
    invalid_feedback.predicted_top1.clear();
    rejected = false;
    try {
        (void)encode(Message{invalid_feedback});
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    auto truncated = bytes;
    truncated.pop_back();
    rejected = false;
    try {
        (void)decode(truncated);
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    auto unknown = bytes;
    unknown[4] = 99;
    rejected = false;
    try {
        (void)decode(unknown);
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    auto oversized = bytes;
    oversized[0] = 0x01;
    oversized[1] = 0x00;
    oversized[2] = 0x10;
    oversized[3] = 0x00;
    rejected = false;
    try {
        (void)decode(oversized);
    } catch (const ProtocolError&) {
        rejected = true;
    }
    ok = ok && rejected;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
