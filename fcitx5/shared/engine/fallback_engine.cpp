#include "engine/fallback_engine.hpp"

#include <utility>

namespace ime::fcitx5 {

FallbackEngine::FallbackEngine(std::filesystem::path table_path) : table_(std::move(table_path)) {}

std::vector<CandidatePrediction> FallbackEngine::predict(const CompositionBuffer& buffer) const {
    std::vector<CandidatePrediction> predictions;
    for (const auto& segment : buffer.segments()) {
        CandidatePrediction prediction;
        prediction.bopomofo = segment.reading();
        if (segment.complete()) {
            prediction.candidates = table_.lookup(prediction.bopomofo);
        } else {
            prediction.raw_text = prediction.bopomofo;
        }
        predictions.push_back(std::move(prediction));
    }
    return predictions;
}

}  // namespace ime::fcitx5
