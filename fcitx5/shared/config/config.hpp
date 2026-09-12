#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace ime::fcitx5 {

struct Config {
    std::string model_path;
    int context_length = 512;
    int thread_count = 1;
    int gpu_layers = 0;
    int idle_timeout_seconds = 1800;
    std::string keyboard_layout = "standard";
    std::string selection_keys = "1234567890";
    int selection_key_count = 10;
    int candidate_page_size = 10;
    std::string candidate_layout = "not_set";
    bool space_selects_candidate = true;
    std::string select_phrase = "before_cursor";
    bool move_cursor_after_selection = false;
    bool esc_clears_entire_buffer = false;
    // CapsLock on still inputs bopomofo (MS IME style).
    bool caps_lock_inputs_bopomofo = true;
    std::string shift_letter_keys = "directly_output_uppercase";
    // Smart Chinese-English: lowercase letters are held raw as a pending word
    // until a tone key or space decides whether they were 注音 or English.
    bool smart_english = false;
    // Retained history in UTF-16 units, including client surrounding text.
    // 0 disables commit tracking and bounds client text by context_length;
    // positive values retain history independently of the request window.
    int context_history_limit = 1024;
    // When the input context loses focus the cache is cleared so text from
    // one field never leaks into the next.
    bool reset_context_on_focus_out = true;
    // Heuristically track edits the client does not reflect back through
    // surrounding text: pass-through Backspace/Delete, caret navigation, and
    // undo/cut/paste/select-all shortcuts clear the cache. Backspace may remove
    // a selection or grapheme, so its effect cannot be inferred safely.
    bool context_edit_tracking = true;
    // Read the text before the caret from the focused widget through the AT-SPI2
    // accessibility bus. This is the authoritative source when the client never
    // pushes surrounding text; commit tracking stays as the fallback.
    bool use_accessibility_context = true;
};

Config default_config();
Config load_config();
nlohmann::json to_json(const Config& cfg);
Config config_from_json(const nlohmann::json& json);
std::filesystem::path config_path();
std::filesystem::path legacy_config_path();
std::filesystem::path runtime_dir();
std::filesystem::path socket_path();
std::filesystem::path pid_path();

}  // namespace ime::fcitx5
