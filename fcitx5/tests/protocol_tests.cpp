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

    PredictRequest request;
    for (std::size_t i = 0; i < request.session_id.size(); ++i) request.session_id[i] = static_cast<std::uint8_t>(i + 1);
    request.request_id = 0x0102030405060708ULL;
    request.buffer_revision = 0x1112131415161718ULL;
    request.context = u"你";
    request.padding.push_back(PaddingEntry{true, {}, U'好'});
    const auto bytes = encode(Message{request});
    ok = ok && equal(bytes, {48, 0, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                             8, 7, 6, 5, 4, 3, 2, 1, 24, 23, 22, 21, 20, 19, 18, 17, 1, 0, 0, 0,
                             96, 79, 1, 0, 0, 0, 1, 125, 89, 0, 0});
    const auto decoded = decode(bytes);
    const auto* decoded_request = std::get_if<PredictRequest>(&decoded);
    ok = ok && decoded_request != nullptr && decoded_request->request_id == request.request_id &&
         decoded_request->buffer_revision == request.buffer_revision && decoded_request->padding.size() == 1 &&
         decoded_request->padding.front().chosen && decoded_request->padding.front().chosen_char == U'好';

    auto trailing = bytes;
    trailing.push_back(0);
    bool rejected = false;
    try {
        (void)decode(trailing);
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
