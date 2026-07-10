#pragma once

#include "bopomofo/table_engine.hpp"
#include "buffer/composition_buffer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ime::fcitx5 {

struct CandidatePrediction {
    std::u16string bopomofo;
    std::u16string raw_text;
    std::vector<char32_t> candidates;
};

class FallbackEngine {
public:
    explicit FallbackEngine(std::filesystem::path table_path);

    std::vector<CandidatePrediction> predict(const CompositionBuffer& buffer) const;

private:
    TableEngine table_;
};

}  // namespace ime::fcitx5
