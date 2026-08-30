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
    // How much committed text the engine remembers for prediction context.
    // 0 disables the self-managed cache; the model only sees surrounding text
    // the client supplies.
    int context_history_limit = 1024;
    // When the input context loses focus the cache is cleared so text from
    // one field never leaks into the next.
    bool reset_context_on_focus_out = true;
    // Heuristically pop the cache when the user presses Backspace outside the
    // composition (the client may not reflect the deletion back to us).
    bool track_context_backspace = false;
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
