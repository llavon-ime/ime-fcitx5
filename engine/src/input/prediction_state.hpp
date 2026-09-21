#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protocol/protocol.hpp"

namespace llavon::ime {

// Async prediction bookkeeping for one input session. The engine drives the
// transport; this type owns the decisions about when a request may start, when
// a response is stale, and what a finished request must clear.
struct PredictionState {
    protocol::SessionId session_id{};
    std::uint64_t next_request_id = 1;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> inflight_request_id;
    std::uint64_t inflight_revision = 0;
    std::u16string key;
    std::size_t revision = 0;
    std::vector<std::size_t> segment_indices;
    bool pending = false;
    bool dirty = false;

    bool session_open() const { return !protocol::is_zero(session_id); }

    // Marks that the buffer changed while a request was in flight.
    void mark_dirty() {
        if (pending) dirty = true;
    }

    // Invalidates every in-flight request; late responses are ignored.
    void invalidate() {
        ++generation;
        clear_pending();
    }

    void clear_pending() {
        pending = false;
        dirty = false;
        inflight_request_id.reset();
        inflight_revision = 0;
        segment_indices.clear();
    }

    // Starts bookkeeping for a request over the completed segments. Returns
    // false when nothing can be requested now; a request already in flight is
    // marked dirty so it repeats once the in-flight one finishes.
    bool begin(std::vector<std::size_t> completed, std::u16string raw_composition, std::size_t buffer_revision) {
        if (pending) {
            dirty = true;
            return false;
        }
        if (completed.empty()) return false;

        segment_indices = std::move(completed);
        pending = true;
        dirty = false;
        key = std::move(raw_composition);
        revision = buffer_revision;
        return true;
    }

    // Correlation check: the response belongs to the in-flight request.
    bool correlates(const protocol::Prediction& prediction) const {
        return inflight_request_id && *inflight_request_id == prediction.request_id &&
               inflight_revision == prediction.buffer_revision && prediction.session_id == session_id;
    }

    bool correlates(const protocol::Error& error) const {
        return !inflight_request_id || error.request_id == 0 || error.request_id == *inflight_request_id;
    }

    // Freshness check: the composition the request was built for is unchanged,
    // so the returned candidates still describe it.
    bool matches_composition(const protocol::Prediction& prediction, std::u16string_view raw_composition,
                             std::size_t buffer_revision) const {
        return prediction.candidates.size() == segment_indices.size() && key == raw_composition &&
               revision == buffer_revision;
    }

    // Finishes the in-flight request. Returns true when the buffer changed
    // while it was pending and a fresh request is required.
    bool finish() {
        const bool was_dirty = dirty;
        clear_pending();
        return was_dirty;
    }
};

}  // namespace llavon::ime
