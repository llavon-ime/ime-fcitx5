#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bopomofo/keymap.hpp"

namespace ime::fcitx5 {

enum class MixedSegmentKind {
    Latin,
    Bopomofo,
    Number,
    Symbol,
};

struct MixedSegment {
    MixedSegmentKind kind = MixedSegmentKind::Symbol;
    size_t begin = 0;
    size_t end = 0;
    std::u16string raw;
    std::u16string body_keys;
    char32_t tone_key = 0;
    std::u16string reading;
    std::vector<char32_t> candidates;
    double score = 0;
};

struct MixedPath {
    std::vector<MixedSegment> segments;
    std::u16string rendered;
    double score = 0;
};

struct MixedDecodeResult {
    std::u16string raw;
    std::vector<MixedPath> paths;
};

// One displayed candidate entry: which path it came from, which character
// candidate of the final Bopomofo segment was chosen, and the complete text.
struct MixedCandidateEntry {
    size_t path_index = 0;
    size_t char_index = 0;
    std::u16string text;
};

// Decodes the complete pending input into complete output paths. The decoder
// has no fcitx runtime dependency: all data (reading lookup and Latin
// frequency) is supplied by callbacks.
//
// Invariants every returned path satisfies:
//   - every path covers the raw input exactly once, without gaps or rewrites;
//   - the exact raw ASCII path is always the first path;
//   - no two paths render the same output text;
//   - paths are ordered by descending score after the raw path.
class MixedInputDecoder {
public:
    using LookupFn = std::function<std::vector<char32_t>(std::u16string_view)>;
    using FrequencyFn = std::function<double(std::u16string_view)>;

    MixedInputDecoder(LookupFn lookup, FrequencyFn frequency);

    MixedDecodeResult decode(std::u16string_view raw, BopomofoKeyboardLayout layout, bool space_tone) const;

    // Expands `result.paths` into displayable candidate entries. The raw path
    // stays first; the best Chinese-closing path (the one ending with the
    // longest suffix) then renders once per character candidate of its final
    // Bopomofo segment, up to `page_size` entries in total.
    std::vector<MixedCandidateEntry> expand_candidates(
        const MixedDecodeResult& result,
        size_t page_size,
        std::optional<size_t> preferred_path = std::nullopt) const;

    static constexpr size_t kTopK = 8;
    static constexpr size_t kMaxSyllableKeys = 6;

private:
    LookupFn lookup_;
    FrequencyFn frequency_;
};

}  // namespace ime::fcitx5
