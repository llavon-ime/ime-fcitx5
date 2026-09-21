#pragma once

#include <algorithm>
#include <cstddef>

namespace llavon::ime {

// Page and cursor math for a candidate list. The owner keeps the candidate
// text; this type tracks only which page and row the user is on. The
// configured page size and the number of displayed candidates are passed in
// because they change with the config and the candidate source.
struct CandidateView {
    int page = 0;
    int cursor = 0;
    bool expanded = false;

    void reset() {
        page = 0;
        cursor = 0;
        expanded = false;
    }

    int page_size(int configured_page_size, std::size_t candidate_count) const {
        if (expanded && candidate_count != 0) return static_cast<int>(candidate_count);
        return configured_page_size;
    }

    int page_offset(int configured_page_size, std::size_t candidate_count) const {
        return page * page_size(configured_page_size, candidate_count);
    }

    // Moves the cursor by delta rows, wrapping around inside the current page.
    bool move_cursor(int delta, int configured_page_size, std::size_t candidate_count) {
        if (candidate_count == 0) return false;

        const int size = page_size(configured_page_size, candidate_count);
        if (size <= 0) return false;

        const int begin = page_offset(configured_page_size, candidate_count);
        const int end = std::min(begin + size, static_cast<int>(candidate_count));
        if (begin >= end) return false;

        int next = cursor + delta;
        if (next < begin) next = end - 1;
        if (next >= end) next = begin;
        if (next == cursor) return false;

        cursor = next;
        return true;
    }

    // Turns the page, optionally keeping the row offset within the page.
    bool page_by(int delta, bool preserve_cursor_offset, int configured_page_size, std::size_t candidate_count) {
        if (candidate_count == 0) return false;

        const int size = page_size(configured_page_size, candidate_count);
        const int page_count = static_cast<int>((candidate_count + static_cast<std::size_t>(size) - 1) /
                                                static_cast<std::size_t>(size));
        const int next_page = std::clamp(page + delta, 0, page_count - 1);
        if (next_page == page) return false;

        const int cursor_offset = preserve_cursor_offset ? cursor % size : 0;
        page = next_page;
        if (preserve_cursor_offset) {
            const int begin = page_offset(configured_page_size, candidate_count);
            const int max_index = static_cast<int>(candidate_count) - 1;
            cursor = std::min(begin + cursor_offset, max_index);
        }
        return true;
    }

    bool set_cursor(int index, int configured_page_size, std::size_t candidate_count) {
        if (candidate_count == 0) return false;

        const int max_index = static_cast<int>(candidate_count) - 1;
        const int next = std::clamp(index, 0, max_index);
        if (next == cursor) return false;

        cursor = next;
        clamp(configured_page_size, candidate_count);
        return true;
    }

    // Keeps the cursor inside the list and the page consistent with it.
    void clamp(int configured_page_size, std::size_t candidate_count) {
        if (candidate_count == 0) {
            cursor = 0;
            page = 0;
            return;
        }

        const int max_index = static_cast<int>(candidate_count) - 1;
        cursor = std::clamp(cursor, 0, max_index);

        const int size = page_size(configured_page_size, candidate_count);
        if (size > 0) page = cursor / size;
    }
};

}  // namespace llavon::ime
