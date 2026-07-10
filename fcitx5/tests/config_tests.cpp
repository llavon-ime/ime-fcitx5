#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "config/config.hpp"

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) saved_ = std::string(value);
    }

    ~ScopedEnv() {
        if (saved_) {
            setenv(name_.c_str(), saved_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

}  // namespace

int run_config_tests() {
    bool ok = true;
    auto cfg = ime::fcitx5::default_config();
    ok = ok && cfg.context_length == 512;
    ok = ok && cfg.thread_count >= 1;
    ok = ok && cfg.gpu_layers == 999;
    ok = ok && cfg.idle_timeout_seconds == 1800;
    ok = ok && cfg.keyboard_layout == "standard";
    ok = ok && cfg.selection_keys == "123456789";
    ok = ok && cfg.selection_key_count == 9;
    ok = ok && cfg.candidate_page_size == 9;
    ok = ok && cfg.candidate_layout == "not_set";
    ok = ok && cfg.space_selects_candidate;
    ok = ok && cfg.select_phrase == "before_cursor";
    ok = ok && !cfg.move_cursor_after_selection;
    ok = ok && !cfg.esc_clears_entire_buffer;
    ok = ok && cfg.caps_lock_inputs_bopomofo;

    auto json = ime::fcitx5::to_json(cfg);
    auto roundtrip = ime::fcitx5::config_from_json(json);
    ok = ok && roundtrip.context_length == cfg.context_length;
    ok = ok && roundtrip.idle_timeout_seconds == cfg.idle_timeout_seconds;
    ok = ok && roundtrip.selection_keys == cfg.selection_keys;
    ok = ok && roundtrip.select_phrase == cfg.select_phrase;
    ok = ok && roundtrip.caps_lock_inputs_bopomofo == cfg.caps_lock_inputs_bopomofo;
    ok = ok && ime::fcitx5::socket_path().filename() == "ime.sock";
    ok = ok && ime::fcitx5::pid_path().filename() == "service.pid";

    ScopedEnv xdg_config("XDG_CONFIG_HOME");
    ScopedEnv xdg_runtime("XDG_RUNTIME_DIR");
    ScopedEnv home("HOME");
    ScopedEnv config_override("IME_FCITX5_CONFIG_PATH");
    setenv("XDG_CONFIG_HOME", "", 1);
    setenv("XDG_RUNTIME_DIR", "", 1);
    setenv("HOME", "/tmp/ime-home", 1);
    unsetenv("IME_FCITX5_CONFIG_PATH");
    ok = ok && ime::fcitx5::config_path() == "/tmp/ime-home/.config/fcitx5/conf/llavon-ime.conf";
    ok = ok && ime::fcitx5::legacy_config_path() == "/tmp/ime-home/.config/llavon-ime/config.json";
    ok = ok && ime::fcitx5::runtime_dir() == std::filesystem::temp_directory_path() / "llavon-ime";

    const auto config_root = std::filesystem::temp_directory_path() / "llavon-ime-config-test";
    std::filesystem::remove_all(config_root);
    setenv("XDG_CONFIG_HOME", config_root.c_str(), 1);
    std::filesystem::create_directories(ime::fcitx5::config_path().parent_path());
    {
        std::ofstream output(ime::fcitx5::config_path());
        output << "ModelPath=/tmp/model.gguf\n"
               << "ContextLength=1024\n"
               << "ThreadCount=2\n"
               << "GpuLayers=3\n"
               << "IdleTimeoutSeconds=4\n"
               << "BopomofoKeyboardLayout=standard\n"
               << "SelectionKeys=asdfghjkl\n"
               << "SelectionKeysCount=6\n"
               << "CandidatePageSize=5\n"
               << "CandidateLayout=Horizontal\n"
               << "ChooseCandidateUsingSpace=False\n"
               << "SelectPhrase=after_cursor\n"
               << "MoveCursorAfterSelection=True\n"
               << "EscKeyClearsEntireComposingBuffer=True\n"
               << "CapsLockInputsBopomofo=False\n";
    }
    const auto loaded = ime::fcitx5::load_config();
    ok = ok && loaded.model_path == "/tmp/model.gguf";
    ok = ok && loaded.context_length == 1024;
    ok = ok && loaded.thread_count == 2;
    ok = ok && loaded.gpu_layers == 3;
    ok = ok && loaded.idle_timeout_seconds == 4;
    ok = ok && loaded.keyboard_layout == "standard";
    ok = ok && loaded.selection_keys == "asdfghjkl";
    ok = ok && loaded.selection_key_count == 6;
    ok = ok && loaded.candidate_page_size == 5;
    ok = ok && loaded.candidate_layout == "horizontal";
    ok = ok && !loaded.space_selects_candidate;
    ok = ok && loaded.select_phrase == "after_cursor";
    ok = ok && loaded.move_cursor_after_selection;
    ok = ok && loaded.esc_clears_entire_buffer;
    ok = ok && !loaded.caps_lock_inputs_bopomofo;

    {
        std::ofstream output(ime::fcitx5::config_path());
        output << "ModelPath=\"/Library/Application Support/llavon-ime/models/model.gguf\"\n";
    }
    const auto quoted_space_loaded = ime::fcitx5::load_config();
    ok = ok && quoted_space_loaded.model_path == "/Library/Application Support/llavon-ime/models/model.gguf";

    {
        std::ofstream output(ime::fcitx5::config_path());
        output << "ModelPath=/Library/Application\\ Support/llavon-ime/models/model.gguf\n";
    }
    const auto escaped_space_loaded = ime::fcitx5::load_config();
    ok = ok && escaped_space_loaded.model_path == "/Library/Application Support/llavon-ime/models/model.gguf";

    {
        std::ofstream output(ime::fcitx5::config_path());
        output << "BopomofoKeyboardLayout=標準\n"
               << "SelectionKeys=左手鍵\n"
               << "CandidateLayout=垂直\n"
               << "SelectPhrase=游標後\n"
               << "CapsLockInputsBopomofo=True\n";
    }
    const auto chinese_loaded = ime::fcitx5::load_config();
    ok = ok && chinese_loaded.keyboard_layout == "standard";
    ok = ok && chinese_loaded.selection_keys == "asdfzxcvb";
    ok = ok && chinese_loaded.candidate_layout == "vertical";
    ok = ok && chinese_loaded.select_phrase == "after_cursor";
    ok = ok && chinese_loaded.caps_lock_inputs_bopomofo;

    const auto invalid = ime::fcitx5::config_from_json(nlohmann::json::parse(
        R"({"model_path":"/tmp/valid.gguf","selection_keys":"bad","selection_key_count":99,"candidate_page_size":0,"candidate_layout":"diagonal","space_selects_candidate":"yes","select_phrase":"near_cursor","move_cursor_after_selection":true})"));
    ok = ok && invalid.model_path == "/tmp/valid.gguf";
    ok = ok && invalid.selection_keys == ime::fcitx5::default_config().selection_keys;
    ok = ok && invalid.selection_key_count == ime::fcitx5::default_config().selection_key_count;
    ok = ok && invalid.candidate_page_size == ime::fcitx5::default_config().candidate_page_size;
    ok = ok && invalid.candidate_layout == ime::fcitx5::default_config().candidate_layout;
    ok = ok && invalid.space_selects_candidate == ime::fcitx5::default_config().space_selects_candidate;
    ok = ok && invalid.select_phrase == ime::fcitx5::default_config().select_phrase;
    ok = ok && invalid.move_cursor_after_selection;
    ok = ok && invalid.caps_lock_inputs_bopomofo == ime::fcitx5::default_config().caps_lock_inputs_bopomofo;

    std::filesystem::remove(ime::fcitx5::config_path());
    std::filesystem::create_directories(ime::fcitx5::legacy_config_path().parent_path());
    {
        std::ofstream output(ime::fcitx5::legacy_config_path());
        output << R"({"model_path":"/tmp/legacy.gguf","gpu_layers":7})";
    }
    const auto legacy_loaded = ime::fcitx5::load_config();
    ok = ok && legacy_loaded.model_path == "/tmp/legacy.gguf";
    ok = ok && legacy_loaded.gpu_layers == 7;

    bool malformed_uses_default = false;
    {
        std::ofstream output(ime::fcitx5::config_path());
        output << "not json";
    }
    try {
        const auto malformed = ime::fcitx5::load_config();
        malformed_uses_default = malformed.model_path == ime::fcitx5::default_config().model_path &&
                                 malformed.context_length == ime::fcitx5::default_config().context_length;
    } catch (...) {
        malformed_uses_default = false;
    }
    ok = ok && malformed_uses_default;
    std::filesystem::remove_all(config_root);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
