#include "input/mixed_input_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include "buffer/composition_buffer.hpp"
#include "input/ascii_tokenizer.hpp"
#include "text/utf.hpp"

namespace ime::fcitx5 {

namespace {

// Centralized ranking weights. All ranking decisions live in the decoder.
constexpr double kLatinBase = 10.0;
constexpr double kAlnumBase = 8.0;
constexpr double kNumberBase = 8.0;
constexpr double kSymbolBase = 0.0;
constexpr double kStructuredBoost = 60.0;
constexpr double kKnownWordBoost = 20.0;
constexpr double kExplicitToneBoost = 40.0;
constexpr double kSpaceToneBoost = 25.0;
constexpr double kBodyCharBoost = 6.0;
constexpr double kSwitchPenalty = 12.0;
constexpr double kFragmentPenalty = 6.0;

bool is_explicit_tone_key(char32_t key, BopomofoKeyboardLayout layout) {
    if (layout == BopomofoKeyboardLayout::Hsu) {
        return key == U'd' || key == U'f' || key == U'j' || key == U's';
    }
    const auto symbol = lookup_bopomofo_key(key);
    return symbol.has_value() && is_bopomofo_tone(*symbol) && *symbol != U' ';
}

void append_codepoint(std::u16string& text, char32_t value) {
    if (value <= 0xFFFF) {
        text.push_back(static_cast<char16_t>(value));
        return;
    }
    const char32_t codepoint = value - 0x10000;
    text.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
    text.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
}

std::u16string render_segment(const MixedSegment& segment) {
    if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
        std::u16string text;
        append_codepoint(text, segment.candidates.front());
        return text;
    }
    return segment.raw;
}

}  // namespace

MixedInputDecoder::MixedInputDecoder(LookupFn lookup, FrequencyFn frequency)
    : lookup_(std::move(lookup)), frequency_(std::move(frequency)) {}

MixedDecodeResult MixedInputDecoder::decode(std::u16string_view raw, BopomofoKeyboardLayout layout,
                                            bool space_tone) const {
    const size_t n = raw.size();

    struct Edge {
        size_t end;
        MixedSegment segment;
    };
    std::vector<std::vector<Edge>> edges(n + 1);

    const auto add_edge = [&](size_t begin, size_t end, MixedSegment segment) {
        segment.begin = begin;
        segment.end = end;
        segment.raw.assign(raw.substr(begin, end - begin));
        edges[begin].push_back({end, std::move(segment)});
    };

    // ASCII edges (generic grammar, no hardcoded websites or words).
    for (size_t i = 0; i < n; ++i) {
        for (const auto& token : tokenize_ascii(raw, i)) {
            MixedSegment segment;
            switch (token.kind) {
                case AsciiTokenKind::LatinWord:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kLatinBase;
                    if (frequency_) {
                        segment.score += kKnownWordBoost * frequency_(raw.substr(token.begin, token.end - token.begin));
                    }
                    break;
                case AsciiTokenKind::Alphanumeric:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kAlnumBase;
                    if (frequency_) {
                        segment.score += kKnownWordBoost * frequency_(raw.substr(token.begin, token.end - token.begin));
                    }
                    break;
                case AsciiTokenKind::Identifier:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kStructuredBoost;
                    break;
                case AsciiTokenKind::Email:
                case AsciiTokenKind::Domain:
                case AsciiTokenKind::URL:
                case AsciiTokenKind::FilesystemPath:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kStructuredBoost;
                    break;
                case AsciiTokenKind::Number:
                    segment.kind = MixedSegmentKind::Number;
                    segment.score = kNumberBase;
                    break;
                case AsciiTokenKind::OperatorOrSymbol:
                    segment.kind = MixedSegmentKind::Symbol;
                    segment.score = kSymbolBase;
                    break;
            }
            add_edge(i, token.end, std::move(segment));
        }
    }

    // Strict Bopomofo edges: one trigger completes exactly one syllable.
    const auto replay = [&](std::u16string_view body, char32_t tone_key) -> std::pair<std::u16string, std::vector<char32_t>> {
        CompositionBuffer scratch;
        const auto result = scratch.add_bopomofo_keys(body, tone_key, layout, true);
        if (!result || !result->completed || scratch.segments().size() != 1) return {};
        const auto reading = scratch.segments().front().reading();
        return {reading, lookup_(reading)};
    };

    for (size_t i = 0; i < n; ++i) {
        for (size_t len = 2; len <= kMaxSyllableKeys && i + len <= n; ++len) {
            const size_t j = i + len;
            const char32_t tone_key = raw[j - 1];
            if (!is_explicit_tone_key(tone_key, layout)) continue;
            const auto body = raw.substr(i, len - 1);
            auto [reading, candidates] = replay(body, tone_key);
            if (candidates.empty()) continue;
            MixedSegment segment;
            segment.kind = MixedSegmentKind::Bopomofo;
            segment.body_keys.assign(body);
            segment.tone_key = tone_key;
            segment.reading = std::move(reading);
            segment.candidates = std::move(candidates);
            // Longer syllables rank above shorter suffixes on the same input.
            segment.score = kExplicitToneBoost + kBodyCharBoost * static_cast<double>(body.size());
            add_edge(i, j, std::move(segment));
        }
    }

    // Space may close a first-tone syllable. The first-tone interpretation
    // only applies at the end of the pending input, and a suffix syllable
    // needs at least two body keys (a single leftover key stays English).
    if (space_tone) {
        for (size_t i = 0; i < n; ++i) {
            const size_t body_len = n - i;
            if (body_len < (i == 0 ? 1 : 2)) continue;
            auto [reading, candidates] = replay(raw.substr(i), U' ');
            if (candidates.empty()) continue;
            MixedSegment segment;
            segment.kind = MixedSegmentKind::Bopomofo;
            segment.body_keys.assign(raw.substr(i));
            segment.tone_key = U' ';
            segment.reading = std::move(reading);
            segment.candidates = std::move(candidates);
            segment.score = kSpaceToneBoost + kBodyCharBoost * static_cast<double>(body_len);
            add_edge(i, n, std::move(segment));
        }
    }

    // Dynamic programming over the DAG, keeping the top-K paths per vertex.
    // Paths are immutable link chains so extending a path is O(1); the full
    // segment list is materialized only for the surviving complete paths.
    struct PathLink;
    using PathLinkPtr = std::shared_ptr<const PathLink>;
    struct PathLink {
        PathLinkPtr prev;
        MixedSegment segment;
        double score;
        std::u16string rendered;
        size_t latin_count = 0;
    };
    struct Entry {
        double score = 0;
        PathLinkPtr path;
    };
    std::vector<std::vector<Entry>> dp(n + 1);
    dp[0].push_back({0, nullptr});

    const auto insert_entry = [&](std::vector<Entry>& bucket, Entry entry) {
        // Paths that render the same complete output are interchangeable at
        // every vertex; keeping only the best prevents ASCII-only clutter
        // from evicting mixed interpretations.
        for (auto& existing : bucket) {
            if (existing.path->rendered == entry.path->rendered) {
                if (entry.score > existing.score) existing = std::move(entry);
                return;
            }
        }
        bucket.push_back(std::move(entry));
        std::sort(bucket.begin(), bucket.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
        if (bucket.size() > kTopK) bucket.resize(kTopK);
    };

    for (size_t v = 0; v < n; ++v) {
        for (const auto& edge : edges[v]) {
            for (const auto& entry : dp[v]) {
                double score = entry.score + edge.segment.score;
                const size_t prev_latin = entry.path ? entry.path->latin_count : 0;
                if (entry.path && entry.path->segment.kind != edge.segment.kind) score -= kSwitchPenalty;
                // Each Latin segment beyond the first is fragmentation; the
                // penalty is incremental so it is never double-counted.
                if (prev_latin > 0 && edge.segment.kind == MixedSegmentKind::Latin) score -= kFragmentPenalty;

                std::u16string rendered = entry.path ? entry.path->rendered : std::u16string();
                rendered += render_segment(edge.segment);
                const size_t latin_count =
                    prev_latin + (edge.segment.kind == MixedSegmentKind::Latin ? 1 : 0);
                auto link = std::make_shared<PathLink>(
                    PathLink{entry.path, edge.segment, score, std::move(rendered), latin_count});
                insert_entry(dp[edge.end], {score, std::move(link)});
            }
        }
    }

    const auto materialize = [](PathLinkPtr link) {
        std::vector<MixedSegment> segments;
        std::vector<const PathLink*> chain;
        for (auto current = link.get(); current; current = current->prev.get()) chain.push_back(current);
        segments.reserve(chain.size());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) segments.push_back((*it)->segment);
        return segments;
    };

    MixedDecodeResult result;
    result.raw.assign(raw);

    // The exact raw ASCII path is always present and always listed first.
    MixedPath raw_path;
    raw_path.score = 0;
    MixedSegment raw_segment;
    raw_segment.kind = MixedSegmentKind::Symbol;
    raw_segment.begin = 0;
    raw_segment.end = n;
    raw_segment.raw.assign(raw);
    raw_path.segments.push_back(std::move(raw_segment));
    raw_path.rendered.assign(raw);
    result.paths.push_back(std::move(raw_path));

    std::sort(dp[n].begin(), dp[n].end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
    std::unordered_set<std::u16string> seen;
    seen.insert(result.paths.front().rendered);
    for (auto& entry : dp[n]) {
        if (!seen.insert(entry.path->rendered).second) continue;
        MixedPath path;
        path.segments = materialize(entry.path);
        path.rendered = entry.path->rendered;
        path.score = entry.score;
        result.paths.push_back(std::move(path));
    }
    return result;
}

std::vector<MixedCandidateEntry> MixedInputDecoder::expand_candidates(
    const MixedDecodeResult& result, size_t page_size, std::optional<size_t> preferred_path) const {
    std::vector<MixedCandidateEntry> entries;
    if (page_size == 0 || result.paths.empty()) return entries;

    entries.push_back({0, 0, result.raw});
    std::unordered_set<std::u16string> seen;
    seen.insert(result.raw);

    // The candidates show the best mixed interpretation: the one closing
    // with the longest Chinese suffix, expanded over its character
    // candidates to fill the page (mirroring suffix-first parsing).
    size_t best_index = std::numeric_limits<size_t>::max();
    size_t best_begin = std::numeric_limits<size_t>::max();
    double best_score = -1;
    if (preferred_path && *preferred_path > 0 && *preferred_path < result.paths.size()) {
        const auto& path = result.paths[*preferred_path];
        if (!path.segments.empty() && path.segments.back().kind == MixedSegmentKind::Bopomofo) {
            best_index = *preferred_path;
        }
    }
    if (best_index == std::numeric_limits<size_t>::max()) {
        for (size_t i = 1; i < result.paths.size(); ++i) {
            const auto& path = result.paths[i];
            if (path.segments.empty() || path.segments.back().kind != MixedSegmentKind::Bopomofo) continue;
            const size_t begin = path.segments.back().begin;
            if (begin < best_begin || (begin == best_begin && path.score > best_score)) {
                best_index = i;
                best_begin = begin;
                best_score = path.score;
            }
        }
    }
    if (best_index == std::numeric_limits<size_t>::max()) return entries;

    const auto& path = result.paths[best_index];
    std::u16string prefix;
    const size_t last = path.segments.size() - 1;
    for (size_t i = 0; i < last; ++i) prefix += render_segment(path.segments[i]);

    const auto& tail = path.segments.back();
    for (size_t char_index = 0; char_index < tail.candidates.size() && entries.size() < page_size; ++char_index) {
        std::u16string text = prefix;
        append_codepoint(text, tail.candidates[char_index]);
        if (!seen.insert(text).second) continue;
        entries.push_back({best_index, char_index, std::move(text)});
    }
    return entries;
}

}  // namespace ime::fcitx5
