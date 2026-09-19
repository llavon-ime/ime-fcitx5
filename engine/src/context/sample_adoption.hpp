#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ime::fcitx5 {

// One composing segment as the focused widget can show it. `rendered` is what
// the widget displays once a candidate is visible (for a segment still being
// typed this is the reading itself), `reading` is the raw reading text.
struct PreeditSegmentState {
    std::u16string_view rendered;
    std::u16string_view reading;
};

// Accessibility samples read the focused widget as-is, and clients can insert
// the composing preedit into that widget, so the end of the sample is the
// composition as it looked when the sample was taken. The client is updated
// after a key is handled, so a sampled segment is either a
// candidate rendering exactly as displayed, or a non-empty prefix of the
// reading (the widget may lag several keystrokes behind, and a sample window
// may cut the newest text). Every segment must contribute text; treating a
// missing segment as an empty suffix could erase identical committed text.
//
// This locates the longest suffix of the sample that can be explained as the
// sequence of current composing segments and strips it, keeping the committed
// text before the composition. Returns nullopt when the sample cannot be
// explained that way (for example the sample window was truncated inside a
// segment in a way that does not match the current reading); such a sample
// must not be adopted while composing.
inline std::optional<std::u16string> strip_preedit_suffix(std::u16string_view sample,
                                                          std::span<const PreeditSegmentState> segments) {
    if (segments.empty()) return std::u16string(sample);
    if (sample.empty()) return std::u16string();

    const size_t segment_count = segments.size();
    const size_t sample_size = sample.size();
    const size_t stride = segment_count + 1;

    // can[pos * stride + segment] is true when sample[pos..] can be explained
    // by segments[segment..]. The final segment consumes to the sample end or
    // ends in the middle of the reading currently being typed, but it cannot
    // consume an empty suffix.
    std::vector<bool> can((sample_size + 1) * stride, false);
    for (size_t pos = 0; pos <= sample_size; ++pos) {
        can[pos * stride + segment_count] = (pos == sample_size);
    }

    for (size_t segment = segment_count; segment-- > 0;) {
        const auto& state = segments[segment];
        for (size_t pos = 0; pos <= sample_size; ++pos) {
            const std::u16string_view rest = sample.substr(pos);
            bool value = false;
            for (const std::u16string_view candidate : {state.rendered, state.reading}) {
                if (candidate.empty()) continue;
                if (rest.size() >= candidate.size()) {
                    if (rest.substr(0, candidate.size()) == candidate) {
                        value = can[(pos + candidate.size()) * stride + segment + 1];
                    }
                } else if (!rest.empty() && candidate == state.reading &&
                           candidate.substr(0, rest.size()) == rest) {
                    // The sample ends in the middle of the reading being typed.
                    value = true;
                }
                if (value) break;
            }
            can[pos * stride + segment] = value;
        }
    }

    // A non-empty sample must contain at least part of the composition; an
    // empty suffix is not an explanation, it is just an unparsed tail.
    for (size_t start = 0; start < sample_size; ++start) {
        if (can[start * stride]) return std::u16string(sample.substr(0, start));
    }
    return std::nullopt;
}

}  // namespace ime::fcitx5
