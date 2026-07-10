#include "buffer/composition_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ime::fcitx5 {

namespace {

void append_utf16(std::u16string& text, char32_t value) {
    if (value == 0) return;
    if (value <= 0xFFFF) {
        text.push_back(static_cast<char16_t>(value));
        return;
    }

    const char32_t codepoint = value - 0x10000;
    text.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
    text.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
}

}  // namespace

bool Segment::complete() const noexcept {
    return syllable.complete();
}

bool Segment::empty() const noexcept {
    return syllable.empty();
}

bool Segment::visible_candidate() const noexcept {
    if (literal != 0) return true;
    return complete() && selected_index < candidates.size();
}

char32_t Segment::selected_candidate() const noexcept {
    if (literal != 0) return literal;
    return visible_candidate() ? candidates[selected_index] : 0;
}

std::u16string Segment::reading() const {
    if (literal != 0) return {};
    return syllable.text();
}

std::u16string Segment::rendered_text() const {
    if (!visible_candidate()) return reading();

    std::u16string result;
    append_utf16(result, selected_candidate());
    return result;
}

bool CompositionBuffer::add_bopomofo(char32_t symbol) {
    if (caret_ > 0 && !segments_[caret_ - 1].visible_candidate()) {
        auto& segment = segments_[caret_ - 1];
        if (segment.syllable.accept(symbol) || segment.syllable.overwrite(symbol)) {
            segment.candidates.clear();
            segment.selected_index = 0;
            segment.manually_chosen = false;
            last_edited_segment_ = caret_ - 1;
            touch();
            return true;
        }
    }

    Segment next;
    if (!next.syllable.accept(symbol)) return false;

    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
    last_edited_segment_ = caret_;
    ++caret_;
    touch();
    return true;
}

bool CompositionBuffer::add_literal(char32_t symbol) {
    if (symbol == 0) return false;

    Segment next;
    next.literal = symbol;
    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
    last_edited_segment_.reset();
    ++caret_;
    touch();
    return true;
}

bool CompositionBuffer::backspace() {
    if (segments_.empty() || caret_ == 0) return false;

    auto& segment = segments_[caret_ - 1];
    last_edited_segment_ = caret_ - 1;

    if (segment.visible_candidate()) {
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
        --caret_;
        last_edited_segment_.reset();
        touch();
        return true;
    }

    const bool removed = segment.syllable.pop_back();
    if (segment.empty()) {
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
        --caret_;
        last_edited_segment_.reset();
    } else {
        segment.candidates.clear();
        segment.selected_index = 0;
        segment.manually_chosen = false;
    }
    if (removed) touch();
    return removed;
}

bool CompositionBuffer::move_cursor_left() {
    if (caret_ == 0) return false;
    --caret_;
    last_edited_segment_.reset();
    touch();
    return true;
}

bool CompositionBuffer::move_cursor_right() {
    if (caret_ >= segments_.size()) return false;
    ++caret_;
    last_edited_segment_.reset();
    touch();
    return true;
}

void CompositionBuffer::clear() {
    if (segments_.empty() && caret_ == 0) return;
    segments_.clear();
    caret_ = 0;
    last_edited_segment_.reset();
    touch();
}

bool CompositionBuffer::empty() const noexcept {
    return segments_.empty();
}

bool CompositionBuffer::has_unfinished_reading() const noexcept {
    return std::any_of(segments_.begin(), segments_.end(), [](const Segment& segment) {
        return !segment.visible_candidate() && !segment.empty();
    });
}

std::u16string CompositionBuffer::raw_composition() const {
    std::u16string result;
    for (const auto& segment : segments_) result += segment.reading();
    return result;
}

std::u16string CompositionBuffer::rendered_composition() const {
    std::u16string result;
    for (const auto& segment : segments_) result += segment.rendered_text();
    return result;
}

std::u16string CompositionBuffer::rendered_prefix_before_caret() const {
    std::u16string result;
    for (size_t i = 0; i < std::min(caret_, segments_.size()); ++i) result += segments_[i].rendered_text();
    return result;
}

std::u16string CompositionBuffer::commit_text() const {
    return rendered_composition();
}

std::u16string CompositionBuffer::candidate_commit_text() const {
    std::u16string result;
    for (const auto& segment : segments_) {
        if (segment.visible_candidate()) result += segment.rendered_text();
    }
    return result;
}

std::optional<size_t> CompositionBuffer::candidate_target(CandidateTarget target) const {
    if (segments_.empty()) return std::nullopt;

    if (target == CandidateTarget::BeforeCursor) {
        if (caret_ == 0) return std::nullopt;
        const size_t index = caret_ - 1;
        if (segments_[index].complete()) return index;
        return std::nullopt;
    }

    if (caret_ >= segments_.size()) return std::nullopt;
    if (segments_[caret_].complete()) return caret_;
    return std::nullopt;
}

std::optional<size_t> CompositionBuffer::last_edited_segment() const noexcept {
    return last_edited_segment_;
}

size_t CompositionBuffer::caret() const noexcept {
    return caret_;
}

size_t CompositionBuffer::revision() const noexcept {
    return revision_;
}

const std::vector<Segment>& CompositionBuffer::segments() const noexcept {
    return segments_;
}

std::vector<size_t> CompositionBuffer::completed_segment_indices() const {
    std::vector<size_t> indices;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].complete()) indices.push_back(i);
    }
    return indices;
}

bool CompositionBuffer::segment_complete(size_t index) const {
    return index < segments_.size() && segments_[index].complete();
}

std::u16string CompositionBuffer::segment_reading(size_t index) const {
    if (index >= segments_.size()) return {};
    return segments_[index].reading();
}

const std::vector<char32_t>* CompositionBuffer::segment_candidates(size_t index) const {
    if (index >= segments_.size()) return nullptr;
    return &segments_[index].candidates;
}

std::optional<size_t> CompositionBuffer::segment_selected_index(size_t index) const {
    if (index >= segments_.size()) return std::nullopt;
    if (!segments_[index].visible_candidate()) return std::nullopt;
    return segments_[index].selected_index;
}

bool CompositionBuffer::set_segment_candidates(size_t index, std::vector<char32_t> candidates,
                                               bool preserve_manual_choice) {
    if (index >= segments_.size()) return false;

    auto& segment = segments_[index];
    if (preserve_manual_choice && segment.manually_chosen) return false;

    segment.candidates = std::move(candidates);
    segment.selected_index = 0;
    segment.manually_chosen = false;
    touch();
    return true;
}

bool CompositionBuffer::select_candidate(size_t segment_index, size_t candidate_index,
                                         bool move_cursor_after_selection) {
    if (segment_index >= segments_.size()) return false;

    auto& segment = segments_[segment_index];
    if (candidate_index >= segment.candidates.size()) return false;

    segment.selected_index = candidate_index;
    segment.manually_chosen = true;
    if (move_cursor_after_selection) caret_ = segment_index + 1;
    last_edited_segment_ = segment_index;
    touch();
    return true;
}

bool CompositionBuffer::cancel_candidate_selection(size_t segment_index) {
    if (segment_index >= segments_.size()) return false;

    auto& segment = segments_[segment_index];
    if (!segment.manually_chosen && segment.selected_index == 0) return false;

    segment.selected_index = 0;
    segment.manually_chosen = false;
    last_edited_segment_ = segment_index;
    touch();
    return true;
}

bool CompositionBuffer::remove_segment(size_t index) {
    if (index >= segments_.size()) return false;

    segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(index));
    if (caret_ > index) --caret_;
    if (caret_ > segments_.size()) caret_ = segments_.size();
    last_edited_segment_.reset();
    touch();
    return true;
}

void CompositionBuffer::touch() {
    ++revision_;
}

}  // namespace ime::fcitx5
