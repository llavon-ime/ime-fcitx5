#pragma once

#include "protocol/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ime::fcitx5 {

using ByteVector = std::vector<std::uint8_t>;

ByteVector encode_message(const PredictRequest& request);
ByteVector encode_message(const PredictResponse& response);
ByteVector encode_message(const StatusResponse& response);
ByteVector encode_message(const ControlRequest& request);

PredictRequest decode_predict_request(const ByteVector& bytes);
PredictResponse decode_predict_response(const ByteVector& bytes);
StatusResponse decode_status_response(const ByteVector& bytes);
ControlRequest decode_control_request(const ByteVector& bytes);
std::string decode_message_type(const ByteVector& bytes);

}  // namespace ime::fcitx5
