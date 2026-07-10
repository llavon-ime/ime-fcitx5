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
    std::string selection_keys = "123456789";
    int selection_key_count = 9;
    int candidate_page_size = 9;
    std::string candidate_layout = "not_set";
    bool space_selects_candidate = true;
    std::string select_phrase = "before_cursor";
    bool move_cursor_after_selection = false;
    bool esc_clears_entire_buffer = false;
    bool caps_lock_inputs_bopomofo = true;
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
