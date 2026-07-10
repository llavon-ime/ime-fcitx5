#include "config/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ime::fcitx5 {

namespace {

const char* non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool allowed_value(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

std::string string_field(const nlohmann::json& json, const char* name, const std::string& fallback,
                         std::initializer_list<std::string_view> allowed = {}) {
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_string()) return fallback;

    const auto value = json.at(name).get<std::string>();
    if (!allowed.size() || allowed_value(value, allowed)) return value;
    return fallback;
}

int int_field(const nlohmann::json& json, const char* name, int fallback, int minimum, int maximum) {
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_number_integer()) return fallback;

    const int value = json.at(name).get<int>();
    if (value < minimum || value > maximum) return fallback;
    return value;
}

bool bool_field(const nlohmann::json& json, const char* name, bool fallback) {
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_boolean()) return fallback;
    return json.at(name).get<bool>();
}

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string unescape_ini_value(std::string value) {
    const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
    std::string_view input(value);
    if (quoted) input = input.substr(1, input.size() - 2);

    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\' || i + 1 >= input.size()) {
            result.push_back(input[i]);
            continue;
        }

        const char escaped = input[++i];
        switch (escaped) {
            case '\\':
                result.push_back('\\');
                break;
            case '"':
                result.push_back('"');
                break;
            case ' ':
                result.push_back(' ');
                break;
            case 'n':
                if (quoted) {
                    result.push_back('\n');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'f':
                if (quoted) {
                    result.push_back('\f');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'r':
                if (quoted) {
                    result.push_back('\r');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 't':
                if (quoted) {
                    result.push_back('\t');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'v':
                if (quoted) {
                    result.push_back('\v');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            default:
                result.push_back('\\');
                result.push_back(escaped);
                break;
        }
    }
    return result;
}

std::map<std::string, std::string> read_simple_ini(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return {};

    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#' || stripped.front() == '[') continue;

        const auto separator = stripped.find('=');
        if (separator == std::string::npos) continue;

        auto key = trim(std::string_view(stripped).substr(0, separator));
        auto value = trim(std::string_view(stripped).substr(separator + 1));
        if (!key.empty()) fields[std::move(key)] = unescape_ini_value(std::move(value));
    }
    return fields;
}

std::string ini_string_field(const std::map<std::string, std::string>& fields, const char* name,
                             const std::string& fallback, std::initializer_list<std::string_view> allowed = {}) {
    const auto it = fields.find(name);
    if (it == fields.end()) return fallback;
    if (!allowed.size() || allowed_value(it->second, allowed)) return it->second;
    return fallback;
}

int ini_int_field(const std::map<std::string, std::string>& fields, const char* name, int fallback, int minimum,
                  int maximum) {
    const auto it = fields.find(name);
    if (it == fields.end()) return fallback;

    try {
        size_t parsed = 0;
        const int value = std::stoi(it->second, &parsed);
        if (parsed != it->second.size() || value < minimum || value > maximum) return fallback;
        return value;
    } catch (...) {
        return fallback;
    }
}

bool ini_bool_field(const std::map<std::string, std::string>& fields, const char* name, bool fallback) {
    const auto it = fields.find(name);
    if (it == fields.end()) return fallback;

    if (it->second == "True" || it->second == "true" || it->second == "1") return true;
    if (it->second == "False" || it->second == "false" || it->second == "0") return false;
    return fallback;
}

std::string keyboard_layout_from_config(std::string_view value, const std::string& fallback) {
    if (value == "standard" || value == "標準") return "standard";
    return fallback;
}

std::string selection_keys_from_config(std::string_view value, const std::string& fallback) {
    if (value == "123456789" || value == "數字鍵") return "123456789";
    if (value == "asdfghjkl" || value == "本位列") return "asdfghjkl";
    if (value == "asdfzxcvb" || value == "左手鍵") return "asdfzxcvb";
    return fallback;
}

std::string candidate_layout_from_config(std::string_view value, const std::string& fallback) {
    if (value == "NotSet" || value == "not_set") return "not_set";
    if (value == "Vertical" || value == "vertical") return "vertical";
    if (value == "Horizontal" || value == "horizontal") return "horizontal";
    if (value == "系統預設") return "not_set";
    if (value == "垂直") return "vertical";
    if (value == "水平") return "horizontal";
    return fallback;
}

std::string select_phrase_from_config(std::string_view value, const std::string& fallback) {
    if (value == "before_cursor" || value == "游標前") return "before_cursor";
    if (value == "after_cursor" || value == "游標後") return "after_cursor";
    return fallback;
}

std::string installed_default_model_path() {
#ifdef IME_FCITX5_INSTALLED_MODEL_PATH
    const std::filesystem::path path(IME_FCITX5_INSTALLED_MODEL_PATH);
    if (!path.empty() && std::filesystem::exists(path)) return path.string();
#endif
    return {};
}

Config with_default_model(Config cfg) {
    if (cfg.model_path.empty()) cfg.model_path = installed_default_model_path();
    return cfg;
}

Config config_from_fcitx_ini(const std::filesystem::path& path) {
    Config cfg = default_config();
    const auto fields = read_simple_ini(path);
    if (fields.empty()) return cfg;

    cfg.model_path = ini_string_field(fields, "ModelPath", cfg.model_path);
    cfg.context_length = ini_int_field(fields, "ContextLength", cfg.context_length, 1, 1048576);
    cfg.thread_count = ini_int_field(fields, "ThreadCount", cfg.thread_count, 1, 1024);
    cfg.gpu_layers = ini_int_field(fields, "GpuLayers", cfg.gpu_layers, 0, 1024);
    cfg.idle_timeout_seconds = ini_int_field(fields, "IdleTimeoutSeconds", cfg.idle_timeout_seconds, 0, 86400);
    if (const auto it = fields.find("BopomofoKeyboardLayout"); it != fields.end()) {
        cfg.keyboard_layout = keyboard_layout_from_config(it->second, cfg.keyboard_layout);
    }
    if (const auto it = fields.find("SelectionKeys"); it != fields.end()) {
        cfg.selection_keys = selection_keys_from_config(it->second, cfg.selection_keys);
    }
    cfg.selection_key_count = ini_int_field(fields, "SelectionKeysCount", cfg.selection_key_count, 4, 9);
    cfg.candidate_page_size = ini_int_field(fields, "CandidatePageSize", cfg.candidate_page_size, 1, 50);
    if (const auto it = fields.find("CandidateLayout"); it != fields.end()) {
        cfg.candidate_layout = candidate_layout_from_config(it->second, cfg.candidate_layout);
    }
    cfg.space_selects_candidate = ini_bool_field(fields, "ChooseCandidateUsingSpace", cfg.space_selects_candidate);
    if (const auto it = fields.find("SelectPhrase"); it != fields.end()) {
        cfg.select_phrase = select_phrase_from_config(it->second, cfg.select_phrase);
    }
    cfg.move_cursor_after_selection =
        ini_bool_field(fields, "MoveCursorAfterSelection", cfg.move_cursor_after_selection);
    cfg.esc_clears_entire_buffer =
        ini_bool_field(fields, "EscKeyClearsEntireComposingBuffer", cfg.esc_clears_entire_buffer);
    cfg.caps_lock_inputs_bopomofo = ini_bool_field(fields, "CapsLockInputsBopomofo", cfg.caps_lock_inputs_bopomofo);
    return cfg;
}

Config load_legacy_json_config(const std::filesystem::path& path) {
    try {
        std::ifstream input(path);
        if (!input) return default_config();
        nlohmann::json json;
        input >> json;
        return config_from_json(json);
    } catch (...) {
        return default_config();
    }
}

#ifdef __APPLE__
std::filesystem::path macos_fcitx_config_path(const char* filename) {
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "fcitx5" / "conf" / filename;
    }
    return {};
}
#endif

std::filesystem::path fcitx_config_path_for(const char* filename) {
    if (const char* xdg = non_empty_env("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "fcitx5" / "conf" / filename;
    }
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / ".config" / "fcitx5" / "conf" / filename;
    }
    return std::filesystem::path(".") / "fcitx5" / "conf" / filename;
}

std::filesystem::path legacy_json_config_path_for(const char* directory) {
    if (const char* xdg = non_empty_env("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / directory / "config.json";
    }
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / ".config" / directory / "config.json";
    }
    return std::filesystem::path(".") / directory / "config.json";
}

void append_unique_path(std::vector<std::filesystem::path>& paths, std::filesystem::path path) {
    if (path.empty()) return;
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(std::move(path));
}

std::vector<std::filesystem::path> fcitx_config_paths() {
    std::vector<std::filesystem::path> paths;
    append_unique_path(paths, config_path());
#ifdef __APPLE__
    if (!non_empty_env("IME_FCITX5_CONFIG_PATH")) {
        append_unique_path(paths, macos_fcitx_config_path("llavon-ime.conf"));
    }
#endif
    return paths;
}

}  // namespace

Config default_config() {
    Config cfg;
    cfg.model_path = installed_default_model_path();
    cfg.gpu_layers = 999;
    const auto threads = std::thread::hardware_concurrency();
    cfg.thread_count = threads == 0 ? 1 : static_cast<int>(threads);
    return cfg;
}

Config load_config() {
    std::error_code ec;
    for (const auto& path : fcitx_config_paths()) {
        if (std::filesystem::exists(path, ec) && !ec) return with_default_model(config_from_fcitx_ini(path));
        ec.clear();
    }

    if (const auto legacy_path = legacy_config_path(); std::filesystem::exists(legacy_path, ec) && !ec)
        return with_default_model(load_legacy_json_config(legacy_path));
    ec.clear();
    return default_config();
}

nlohmann::json to_json(const Config& cfg) {
    return nlohmann::json{
        {"model_path", cfg.model_path},
        {"context_length", cfg.context_length},
        {"thread_count", cfg.thread_count},
        {"gpu_layers", cfg.gpu_layers},
        {"idle_timeout_seconds", cfg.idle_timeout_seconds},
        {"keyboard_layout", cfg.keyboard_layout},
        {"selection_keys", cfg.selection_keys},
        {"selection_key_count", cfg.selection_key_count},
        {"candidate_page_size", cfg.candidate_page_size},
        {"candidate_layout", cfg.candidate_layout},
        {"space_selects_candidate", cfg.space_selects_candidate},
        {"select_phrase", cfg.select_phrase},
        {"move_cursor_after_selection", cfg.move_cursor_after_selection},
        {"esc_clears_entire_buffer", cfg.esc_clears_entire_buffer},
        {"caps_lock_inputs_bopomofo", cfg.caps_lock_inputs_bopomofo},
    };
}

Config config_from_json(const nlohmann::json& json) {
    Config cfg = default_config();
    if (!json.is_object()) return cfg;

    cfg.model_path = string_field(json, "model_path", cfg.model_path);
    cfg.context_length = int_field(json, "context_length", cfg.context_length, 1, 1048576);
    cfg.thread_count = int_field(json, "thread_count", cfg.thread_count, 1, 1024);
    cfg.gpu_layers = int_field(json, "gpu_layers", cfg.gpu_layers, 0, 1024);
    cfg.idle_timeout_seconds = int_field(json, "idle_timeout_seconds", cfg.idle_timeout_seconds, 0, 86400);
    cfg.keyboard_layout =
        keyboard_layout_from_config(string_field(json, "keyboard_layout", cfg.keyboard_layout), cfg.keyboard_layout);
    cfg.selection_keys =
        selection_keys_from_config(string_field(json, "selection_keys", cfg.selection_keys), cfg.selection_keys);
    cfg.selection_key_count = int_field(json, "selection_key_count", cfg.selection_key_count, 4, 9);
    cfg.candidate_page_size = int_field(json, "candidate_page_size", cfg.candidate_page_size, 1, 50);
    cfg.candidate_layout = candidate_layout_from_config(
        string_field(json, "candidate_layout", cfg.candidate_layout), cfg.candidate_layout);
    cfg.space_selects_candidate = bool_field(json, "space_selects_candidate", cfg.space_selects_candidate);
    cfg.select_phrase =
        select_phrase_from_config(string_field(json, "select_phrase", cfg.select_phrase), cfg.select_phrase);
    cfg.move_cursor_after_selection = bool_field(json, "move_cursor_after_selection", cfg.move_cursor_after_selection);
    cfg.esc_clears_entire_buffer = bool_field(json, "esc_clears_entire_buffer", cfg.esc_clears_entire_buffer);
    cfg.caps_lock_inputs_bopomofo = bool_field(json, "caps_lock_inputs_bopomofo", cfg.caps_lock_inputs_bopomofo);
    return cfg;
}

std::filesystem::path config_path() {
    if (const char* override = non_empty_env("IME_FCITX5_CONFIG_PATH")) {
        return override;
    }
    return fcitx_config_path_for("llavon-ime.conf");
}

std::filesystem::path legacy_config_path() {
    return legacy_json_config_path_for("llavon-ime");
}

std::filesystem::path runtime_dir() {
    if (const char* xdg = non_empty_env("XDG_RUNTIME_DIR")) {
        return std::filesystem::path(xdg) / "llavon-ime";
    }
    return std::filesystem::temp_directory_path() / "llavon-ime";
}

std::filesystem::path socket_path() {
    return runtime_dir() / "ime.sock";
}

std::filesystem::path pid_path() {
    return runtime_dir() / "service.pid";
}

}  // namespace ime::fcitx5
