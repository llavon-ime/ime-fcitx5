#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "bopomofo/syllable.hpp"

namespace ime::fcitx5 {

enum class CandidateTarget {
    BeforeCursor,
    AfterCursor,
};

struct Segment {
    Syllable syllable;
    char32_t literal = 0;
    std::vector<char32_t> candidates;
    size_t selected_index = 0;
    bool manually_chosen = false;

    bool complete() const noexcept;
    bool empty() const noexcept;
    bool visible_candidate() const noexcept;
    char32_t selected_candidate() const noexcept;
    std::u16string reading() const;
    std::u16string rendered_text() const;
};

class CompositionBuffer {
public:
    bool add_bopomofo(char32_t symbol);
    bool add_literal(char32_t symbol);
    bool backspace();
    bool move_cursor_left();
    bool move_cursor_right();
    void clear();

    bool empty() const noexcept;
    bool has_unfinished_reading() const noexcept;
    std::u16string raw_composition() const;
    std::u16string rendered_composition() const;
    std::u16string rendered_prefix_before_caret() const;
    std::u16string commit_text() const;
    std::u16string candidate_commit_text() const;
    std::optional<size_t> candidate_target(CandidateTarget target) const;
    std::optional<size_t> last_edited_segment() const noexcept;
    size_t caret() const noexcept;
    size_t revision() const noexcept;
    const std::vector<Segment>& segments() const noexcept;
    std::vector<size_t> completed_segment_indices() const;

    bool segment_complete(size_t index) const;
    std::u16string segment_reading(size_t index) const;
    const std::vector<char32_t>* segment_candidates(size_t index) const;
    std::optional<size_t> segment_selected_index(size_t index) const;
    bool set_segment_candidates(size_t index, std::vector<char32_t> candidates, bool preserve_manual_choice = true);
    bool select_candidate(size_t segment_index, size_t candidate_index, bool move_cursor_after_selection);
    bool cancel_candidate_selection(size_t segment_index);
    bool remove_segment(size_t index);

private:
    void touch();

    std::vector<Segment> segments_;
    size_t caret_ = 0;
    size_t revision_ = 0;
    std::optional<size_t> last_edited_segment_;
};

}  // namespace ime::fcitx5
