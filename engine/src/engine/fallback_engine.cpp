#include "engine/fallback_engine.hpp"

#include "text/utf.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace ime::fcitx5 {

FallbackEngine::FallbackEngine(std::filesystem::path table_path) : table_(table_path) {
    std::ifstream input(table_path.parent_path() / "tokens" / "latin.json");
    if (!input) return;

    try {
        const auto json = nlohmann::json::parse(input);
        if (json.empty()) return;

        long long minimum = json.begin().value().get<long long>();
        long long maximum = minimum;
        for (const auto& [word, value] : json.items()) {
            (void)word;
            const long long raw = value.get<long long>();
            minimum = std::min(minimum, raw);
            maximum = std::max(maximum, raw);
        }

        const double range = static_cast<double>(maximum - minimum);
        for (const auto& [word, value] : json.items()) {
            const long long raw = value.get<long long>();
            // Lower values mean higher frequency in the training corpus.
            const double normalized =
                range > 0 ? static_cast<double>(maximum - raw) / range : 1.0;
            english_frequencies_.emplace(utf8_to_u16(word), normalized);
        }
    } catch (...) {
        english_frequencies_.clear();
    }
}

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

std::vector<char32_t> FallbackEngine::lookup(std::u16string_view bopomofo) const {
    return table_.lookup(bopomofo);
}

bool FallbackEngine::is_known_english(std::u16string_view word) const {
    return english_frequencies_.contains(std::u16string(word));
}

double FallbackEngine::latin_frequency(std::u16string_view word) const {
    const auto it = english_frequencies_.find(std::u16string(word));
    if (it == english_frequencies_.end()) return 0.0;
    return it->second;
}

std::vector<char32_t> FallbackEngine::merge_model_candidates(
    const Segment& segment, std::vector<char32_t> model_candidates) const {
    if (!segment.complete()) return model_candidates;

    auto table_candidates = table_.lookup(segment.reading());
    for (const auto& reading : segment.alternative_readings) {
        auto alternative = table_.lookup(reading);
        for (const auto candidate : alternative) table_candidates.push_back(candidate);
    }

    std::unordered_set<char32_t> table_set(table_candidates.begin(), table_candidates.end());
    std::vector<char32_t> merged;
    merged.reserve(table_candidates.size() + model_candidates.size());

    std::unordered_set<char32_t> seen;
    for (const auto candidate : model_candidates) {
        if (table_set.contains(candidate) && seen.insert(candidate).second) {
            merged.push_back(candidate);
        }
    }
    for (const auto candidate : table_candidates) {
        if (seen.insert(candidate).second) merged.push_back(candidate);
    }
    return merged;
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
