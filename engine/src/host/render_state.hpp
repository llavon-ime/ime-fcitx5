#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llavon::ime {

class Config;
class InputSession;
class MixedInputDecoder;

struct PreeditSegment {
    std::u16string text;
    bool underlined = false;
};

// What activating a candidate entry does.
enum class RenderTarget {
    None,
    // Select through InputProcessor::select_candidate.
    Candidates,
    // Select through InputProcessor::select_symbol with `symbol_epoch`.
    SymbolMenu,
    // Informational only (phrase-marking hint); activating it is a no-op.
    MarkingHint,
};

// Everything a host needs to draw the input panel for one context. Built by
// the engine so every frontend renders identical state.
struct RenderState {
    bool composition_empty = true;

    std::vector<PreeditSegment> preedit;
    // Caret position in UTF-16 code units of the concatenated preedit text.
    std::size_t caret = 0;

    std::u16string aux_up;
    std::u16string aux_down;

    bool has_candidates = false;
    RenderTarget candidate_target = RenderTarget::None;
    std::uint64_t symbol_epoch = 0;
    std::vector<std::u16string> candidates;
    int page = 0;
    int page_size = 0;
    int page_count = 0;
    // Cursor index within the current page.
    int cursor = 0;
    // False when the list has no tracked cursor yet (plain candidate lists
    // leave the first row highlighted).
    bool cursor_visible = false;
    std::vector<char32_t> selection_keys;
    // Config::candidate_layout verbatim: "", "vertical", or "horizontal".
    std::string layout_hint;
};

// Concatenates the preedit segments into the displayed composition text.
std::u16string preedit_text(const RenderState& state);

// Builds the render state for a session. Updates the session's candidate view
// and displayed candidate cache, mirroring what the frontends used to do.
RenderState build_render_state(InputSession& session, const Config& config, MixedInputDecoder& decoder);

}  // namespace llavon::ime
