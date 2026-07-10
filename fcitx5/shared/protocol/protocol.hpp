#pragma once

#include <string>
#include <vector>

namespace ime::fcitx5 {

struct PaddingEntry {
    bool chosen = false;
    std::u16string bopomofo;
    char32_t chosen_char = 0;
};

struct PredictRequest {
    std::u16string context;
    std::vector<PaddingEntry> padding;
};

struct PredictResponse {
    std::vector<std::vector<char32_t>> candidates;
};

struct StatusResponse {
    bool running = false;
    bool model_loaded = false;
    std::string backend;
    std::string model_path;
    std::string error;
};

struct ControlRequest {
    std::string operation;
};

}  // namespace ime::fcitx5
