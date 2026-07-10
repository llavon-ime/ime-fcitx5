#include "fcitx5/ime_config.hpp"

namespace ime::fcitx5 {

namespace {

std::string to_string(BopomofoKeyboardLayout value) {
    switch (value) {
        case BopomofoKeyboardLayout::Standard:
            return "standard";
    }
    return "standard";
}

std::string to_string(SelectionKeys value) {
    switch (value) {
        case SelectionKeys::Digits:
            return "123456789";
        case SelectionKeys::HomeRow:
            return "asdfghjkl";
        case SelectionKeys::LeftHand:
            return "asdfzxcvb";
    }
    return "123456789";
}

std::string to_string(CandidateLayout value) {
    switch (value) {
        case CandidateLayout::NotSet:
            return "not_set";
        case CandidateLayout::Vertical:
            return "vertical";
        case CandidateLayout::Horizontal:
            return "horizontal";
    }
    return "not_set";
}

std::string to_string(SelectPhrase value) {
    switch (value) {
        case SelectPhrase::BeforeCursor:
            return "before_cursor";
        case SelectPhrase::AfterCursor:
            return "after_cursor";
    }
    return "before_cursor";
}

BopomofoKeyboardLayout keyboard_layout_from_string(const std::string& value) {
    if (value == "standard" || value == "標準") return BopomofoKeyboardLayout::Standard;
    return BopomofoKeyboardLayout::Standard;
}

SelectionKeys selection_keys_from_string(const std::string& value) {
    if (value == "asdfghjkl" || value == "本位列") return SelectionKeys::HomeRow;
    if (value == "asdfzxcvb" || value == "左手鍵") return SelectionKeys::LeftHand;
    return SelectionKeys::Digits;
}

CandidateLayout candidate_layout_from_string(const std::string& value) {
    if (value == "vertical" || value == "Vertical" || value == "垂直") return CandidateLayout::Vertical;
    if (value == "horizontal" || value == "Horizontal" || value == "水平") return CandidateLayout::Horizontal;
    return CandidateLayout::NotSet;
}

SelectPhrase select_phrase_from_string(const std::string& value) {
    if (value == "after_cursor" || value == "游標後") return SelectPhrase::AfterCursor;
    return SelectPhrase::BeforeCursor;
}

}  // namespace

Config to_shared_config(const ImeFcitxConfig& config) {
    Config shared;
    shared.model_path = *config.modelPath;
    shared.context_length = *config.contextLength;
    shared.thread_count = *config.threadCount;
    shared.gpu_layers = *config.gpuLayers;
    shared.idle_timeout_seconds = *config.idleTimeoutSeconds;
    shared.keyboard_layout = to_string(*config.keyboardLayout);
    shared.selection_keys = to_string(*config.selectionKeys);
    shared.selection_key_count = *config.selectionKeyCount;
    shared.candidate_page_size = *config.candidatePageSize;
    shared.candidate_layout = to_string(*config.candidateLayout);
    shared.space_selects_candidate = *config.chooseCandidateUsingSpace;
    shared.select_phrase = to_string(*config.selectPhrase);
    shared.move_cursor_after_selection = *config.moveCursorAfterSelection;
    shared.esc_clears_entire_buffer = *config.escKeyClearsEntireComposingBuffer;
    shared.caps_lock_inputs_bopomofo = *config.capsLockInputsBopomofo;
    return shared;
}

void apply_shared_config(ImeFcitxConfig& target, const Config& source) {
    (void)target.modelPath.setValue(source.model_path);
    (void)target.contextLength.setValue(source.context_length);
    (void)target.threadCount.setValue(source.thread_count);
    (void)target.gpuLayers.setValue(source.gpu_layers);
    (void)target.idleTimeoutSeconds.setValue(source.idle_timeout_seconds);
    (void)target.keyboardLayout.setValue(keyboard_layout_from_string(source.keyboard_layout));
    (void)target.selectionKeys.setValue(selection_keys_from_string(source.selection_keys));
    (void)target.selectionKeyCount.setValue(source.selection_key_count);
    (void)target.candidatePageSize.setValue(source.candidate_page_size);
    (void)target.candidateLayout.setValue(candidate_layout_from_string(source.candidate_layout));
    (void)target.chooseCandidateUsingSpace.setValue(source.space_selects_candidate);
    (void)target.selectPhrase.setValue(select_phrase_from_string(source.select_phrase));
    (void)target.moveCursorAfterSelection.setValue(source.move_cursor_after_selection);
    (void)target.escKeyClearsEntireComposingBuffer.setValue(source.esc_clears_entire_buffer);
    (void)target.capsLockInputsBopomofo.setValue(source.caps_lock_inputs_bopomofo);
}

}  // namespace ime::fcitx5
