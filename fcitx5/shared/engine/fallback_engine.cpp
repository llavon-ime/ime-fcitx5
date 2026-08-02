#include "engine/fallback_engine.hpp"

#include <unordered_set>
#include <utility>

namespace ime::fcitx5 {

FallbackEngine::FallbackEngine(std::filesystem::path table_path) : table_(std::move(table_path)) {}

std::vector<CandidatePrediction> FallbackEngine::predict(const CompositionBuffer& buffer) const {
    std::vector<CandidatePrediction> predictions;
    for (const auto& segment : buffer.segments()) {
        CandidatePrediction prediction;
        prediction.bopomofo = segment.reading();
        if (segment.complete()) {
            auto candidates = table_.lookup(prediction.bopomofo);
            prediction.candidates = append_alternative_candidates(segment, std::move(candidates));
        } else {
            prediction.raw_text = prediction.bopomofo;
        }
        predictions.push_back(std::move(prediction));
    }
    return predictions;
}

std::vector<char32_t> FallbackEngine::append_alternative_candidates(
    const Segment& segment, std::vector<char32_t> primary_candidates) const {
    for (const auto& reading : segment.alternative_readings) {
        auto alternative = table_.lookup(reading);
        for (const auto candidate : alternative) primary_candidates.push_back(candidate);
    }

    std::unordered_set<char32_t> seen;
    std::vector<char32_t> merged;
    merged.reserve(primary_candidates.size());
    for (const auto candidate : primary_candidates) {
        if (seen.insert(candidate).second) merged.push_back(candidate);
    }
    return merged;
}

}  // namespace ime::fcitx5
