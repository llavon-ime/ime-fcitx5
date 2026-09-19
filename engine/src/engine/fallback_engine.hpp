#pragma once

#include "bopomofo/table_engine.hpp"
#include "buffer/composition_buffer.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    std::vector<char32_t> lookup(std::u16string_view bopomofo) const;
    bool is_known_english(std::u16string_view word) const;
    // Normalized Latin confidence in [0, 1]; 0.0 for words outside the lexicon.
    double latin_frequency(std::u16string_view word) const;
    // Merges table candidates with model-proposed candidates; model choices
    // that exist in the table keep their relative order ahead of the rest.
    std::vector<char32_t> merge_model_candidates(
        const Segment& segment,
        std::vector<char32_t> model_candidates) const;
    std::vector<char32_t> append_alternative_candidates(
        const Segment& segment,
        std::vector<char32_t> primary_candidates) const;

private:
    TableEngine table_;
    std::unordered_map<std::u16string, double> english_frequencies_;
};

}  // namespace ime::fcitx5
