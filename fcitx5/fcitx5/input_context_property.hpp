#pragma once

#include <fcitx/inputcontextproperty.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "input/input_state.hpp"
#include "input/mixed_input_decoder.hpp"
#include "input/pending_token.hpp"
#include "protocol/protocol.hpp"
#include "symbol/symbol_menu.hpp"

namespace ime::fcitx5 {

inline bool prediction_change_requires_request(bool prediction_pending, bool& prediction_dirty) noexcept {
    if (!prediction_pending) return true;
    prediction_dirty = true;
    return false;
}

// Reversible decode state for one pending input. The preview is rendered in
// preedit without destroying the exact raw keys kept by PendingInput.
struct MixedDecisionState {
    MixedDecodeResult result;
    size_t preview_path = 0;
    size_t preview_character = 0;
    uint64_t source_revision = 0;
    bool english_boundary = false;
    bool raw_forced = false;

    bool active() const noexcept { return !result.raw.empty(); }
    void clear() {
        result = MixedDecodeResult{};
        preview_path = 0;
        preview_character = 0;
        source_revision = 0;
        english_boundary = false;
        raw_forced = false;
    }
};

class ImeInputContextProperty final : public fcitx::InputContextProperty {
public:
    ~ImeInputContextProperty() override {
        if (session_close_handle) session_close_handle();
    }

    CompositionBuffer buffer;
    std::vector<std::u16string> displayed_candidates;
    int candidate_page = 0;
    int candidate_cursor = 0;
    bool candidate_expanded = false;
    InputState input_state = InputState::Empty;
    SymbolMenuState symbol_menu;
    PendingInput pending_token;
    MixedDecisionState mixed_decision;

    protocol::SessionId session_id{};
    std::uint64_t next_request_id = 1;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> inflight_request_id;
    std::uint64_t inflight_revision = 0;
    std::u16string prediction_key;
    std::size_t prediction_revision = 0;
    std::vector<std::size_t> inflight_segment_indices;
    bool prediction_pending = false;
    bool prediction_dirty = false;

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::function<void()> session_close_handle;

    void invalidate_generation() {
        ++generation;
        inflight_request_id.reset();
        prediction_pending = false;
        prediction_dirty = false;
        inflight_segment_indices.clear();
    }

    void copyTo(fcitx::InputContextProperty*) override {}
    bool needCopy() const override { return false; }
};

using ImeInputContextPropertyFactory = fcitx::SimpleInputContextPropertyFactory<ImeInputContextProperty>;

}  // namespace ime::fcitx5
