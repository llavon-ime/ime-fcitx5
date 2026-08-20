#include "buffer/composition_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
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
    return reading_finalized && syllable.complete();
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
    if (caret_ > 0 && !segments_[caret_ - 1].visible_candidate() &&
        !segments_[caret_ - 1].reading_finalized) {
        auto& segment = segments_[caret_ - 1];
        if (segment.syllable.accept(symbol) || segment.syllable.overwrite(symbol)) {
            segment.candidates.clear();
            segment.selected_index = 0;
            segment.manually_chosen = false;
            segment.reading_finalized = is_bopomofo_tone(symbol) && segment.syllable.complete();
            segment.alternative_readings.clear();
            last_edited_segment_ = caret_ - 1;
            touch();
            return true;
        }
    }

    Segment next;
    if (!next.syllable.accept(symbol)) return false;
    next.reading_finalized = is_bopomofo_tone(symbol) && next.syllable.complete();

    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
    last_edited_segment_ = caret_;
    ++caret_;
    touch();
    return true;
}

std::optional<BopomofoInputResult> CompositionBuffer::add_bopomofo_key(char32_t key,
                                                                       BopomofoKeyboardLayout layout,
                                                                       bool accept_uppercase) {
    size_t active_index = segments_.size();
    if (caret_ > 0 && !segments_[caret_ - 1].visible_candidate() &&
        !segments_[caret_ - 1].reading_finalized && !segments_[caret_ - 1].empty()) {
        active_index = caret_ - 1;
    }

    Syllable active = active_index < segments_.size() ? segments_[active_index].syllable : Syllable();
    auto result = apply_bopomofo_key(active, layout, key, accept_uppercase);
    if (result.status == BopomofoKeyStatus::Rejected) return std::nullopt;

    if (active_index < segments_.size()) {
        auto& segment = segments_[active_index];
        segment.syllable = std::move(active);
        segment.candidates.clear();
        segment.selected_index = 0;
        segment.manually_chosen = false;
        segment.reading_finalized = result.status == BopomofoKeyStatus::Completed;
        segment.alternative_readings.clear();
        if (result.status == BopomofoKeyStatus::Completed) {
            segment.alternative_readings = std::move(result.alternative_readings);
        }
    } else {
        Segment next;
        next.syllable = std::move(active);
        next.reading_finalized = result.status == BopomofoKeyStatus::Completed;
        if (result.status == BopomofoKeyStatus::Completed) {
            next.alternative_readings = std::move(result.alternative_readings);
        }
        segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
        active_index = caret_;
        ++caret_;
    }
    last_edited_segment_ = active_index;
    touch();
    return BopomofoInputResult{active_index, result.status == BopomofoKeyStatus::Completed};
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
        segment.reading_finalized = false;
        segment.alternative_readings.clear();
    }
    if (removed) touch();
    return removed;
}

bool CompositionBuffer::delete_forward() {
    if (caret_ >= segments_.size()) return false;
    return remove_segment(caret_);
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
        return !segment.reading_finalized && !segment.empty();
    });
}

bool CompositionBuffer::has_unfinished_reading_before_caret() const noexcept {
    if (caret_ == 0 || caret_ > segments_.size()) return false;
    const auto& segment = segments_[caret_ - 1];
    return !segment.reading_finalized && !segment.empty();
}

bool CompositionBuffer::clear_unfinished_reading() {
    const auto unfinished = [](const Segment& segment) {
        return !segment.reading_finalized && !segment.empty();
    };
    if (caret_ > 0 && unfinished(segments_[caret_ - 1])) return remove_segment(caret_ - 1);
    if (caret_ < segments_.size() && unfinished(segments_[caret_])) return remove_segment(caret_);

    const auto it = std::find_if(segments_.begin(), segments_.end(), unfinished);
    if (it == segments_.end()) return false;
    return remove_segment(static_cast<size_t>(std::distance(segments_.begin(), it)));
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

    size_t index = 0;
    if (target == CandidateTarget::BeforeCursor) {
        // Match McBopomofo's actualCandidateCursorIndex(): at the leading
        // boundary there is no segment before the caret, so candidate
        // selection clamps to the first segment instead of disappearing.
        index = caret_ == 0 ? 0 : caret_ - 1;
    } else {
        // Likewise, after-cursor selection at the trailing boundary clamps to
        // the final segment.
        index = caret_ >= segments_.size() ? segments_.size() - 1 : caret_;
    }

    if (index < segments_.size() && segments_[index].complete()) return index;
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

std::optional<size_t> CompositionBuffer::manually_chosen_segment_at_caret() const noexcept {
    if (caret_ > 0 && segments_[caret_ - 1].manually_chosen) return caret_ - 1;
    if (caret_ < segments_.size() && segments_[caret_].manually_chosen) return caret_;
    return std::nullopt;
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
