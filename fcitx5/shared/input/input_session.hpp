#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "context/context_cache.hpp"
#include "input/candidate_view.hpp"
#include "input/input_state.hpp"
#include "input/mixed_input_decoder.hpp"
#include "input/pending_token.hpp"
#include "input/prediction_state.hpp"
#include "protocol/protocol.hpp"
#include "symbol/symbol_menu.hpp"

namespace ime::fcitx5 {

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

// Per-input-context editing state. The engine swaps the whole object in and
// out of the active input context, so every field always travels together.
struct InputSession {
    InputState state;
    CandidateView candidate_view;
    std::vector<std::u16string> displayed_candidates;

    CompositionBuffer buffer;
    SymbolMenuState symbol_menu;
    PendingInput pending_token;
    MixedDecisionState mixed_decision;
    ContextCache context_cache;
    bool client_surrounding_authoritative = false;

    PredictionState prediction;

    InputStateKind kind() const { return input_state_kind(state); }
    bool empty() const { return kind() == InputStateKind::Empty; }
    bool inputting() const { return kind() == InputStateKind::Inputting; }
    bool choosing_candidate() const { return kind() == InputStateKind::ChoosingCandidate; }
};

}  // namespace ime::fcitx5
