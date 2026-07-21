#include <exception>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32

#include <windows.h>

#include "platform/server_strategy.hpp"
#include "utils/runtime_paths.hpp"

namespace {

constexpr const wchar_t* kModelFilename = L"llavon-ime-llama-250m-Q4_K_M.gguf";
constexpr const wchar_t* kModelPathEnv = L"LLAVON_IME_MODEL_PATH";
constexpr const wchar_t* kTablesDirEnv = L"LLAVON_IME_TABLES_DIR";

void print_usage(const char* exe) {
    std::cerr << "Usage: " << exe << " [<model-path> <tables-dir>]\n";
    std::cerr << "When no arguments are provided, paths are resolved from "
                 "LLAVON_IME_MODEL_PATH/LLAVON_IME_TABLES_DIR or the installed layout.\n";
}

std::optional<std::filesystem::path> env_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(value);
}

std::filesystem::path executable_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) throw std::runtime_error("failed to resolve executable path");
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path default_model_path() {
    if (auto path = env_path(kModelPathEnv)) return *path;
    return executable_dir().parent_path() / "models" / kModelFilename;
}

std::filesystem::path default_tables_dir() {
    if (auto path = env_path(kTablesDirEnv)) return *path;
    return executable_dir().parent_path() / "tables";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::clog << "[SRV] IME Service starting\n";
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        std::filesystem::path model_path;
        std::filesystem::path tables_dir;
        if (argc == 1) {
            model_path = default_model_path();
            tables_dir = default_tables_dir();
        } else if (argc == 3) {
            model_path = argv[1];
            tables_dir = argv[2];
        } else {
            print_usage(argv[0]);
            return 1;
        }
        imesvc::RuntimePaths::configure(std::move(model_path), std::move(tables_dir));
        auto server = imesvc::create_server_strategy();
        std::clog << "[SRV] platform server: " << server->name() << '\n';
        return server->run();
    } catch (const std::exception& error) {
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}

#else

#include "platform/server_strategy.hpp"
#include "pipe/protocol.hpp"
#include "platform/unix_socket_server.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace {

void print_usage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "  --model PATH\n"
              << "  --tables PATH\n"
               << "  --context-length N       (default 384; maximum 384)\n"
              << "  --threads N              (default 8)\n"
              << "  --gpu-layers auto|N      (default auto)\n"
              << "  --max-sessions N         (default 8)\n"
              << "  --max-idle-sessions N    (default 4)\n"
              << "  --max-concurrent-predictions N (default 2)\n"
              << "  --idle-timeout N         seconds (default 1800)\n"
               << "  --socket PATH             override runtime socket\n"
                << "  --pid PATH                override PID artifact\n"
                << "  --personal-learning        enable the local feedback database (explicit opt-in)\n"
                << "  --training-data-dir PATH   override the private local feedback directory\n"
                << "  --base-model-sha256 SHA256 authoritative F32 base-model identity for personal learning\n"
                << "  --lora-training            enable automatic local LoRA training (requires the options below)\n"
                << "  --base-safetensors PATH    verified F32 base weights used only by the trainer\n"
                << "  --trainer PATH             llavon-ime-trainer executable\n"
                << "  --adapter-dir PATH         private directory for published LoRA adapters\n";
}

const char* env_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::uint64_t positive_number(std::string_view value, const char* option) {
    if (value.empty() || value.front() == '-') throw std::runtime_error(std::string("invalid ") + option);
    std::size_t parsed = 0;
    unsigned long long number = 0;
    try {
        number = std::stoull(std::string(value), &parsed, 10);
    } catch (...) {
        throw std::runtime_error(std::string("invalid ") + option + ": " + std::string(value));
    }
    if (parsed != value.size() || number > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(std::string("invalid ") + option + ": " + std::string(value));
    }
    return number;
}

std::filesystem::path default_tables_dir(const char* executable) {
    if (const char* value = env_value("LLAVON_IME_TABLES_DIR")) return value;
    if (const char* value = env_value("IME_FCITX5_TABLE_DIR")) return value;

    const auto cwd = std::filesystem::current_path();
    for (const auto& candidate : {cwd / "tables", cwd / "ime-service" / "table"}) {
        if (std::filesystem::is_directory(candidate)) return candidate;
    }

    std::error_code error;
    const auto executable_path = std::filesystem::absolute(executable, error);
    if (!error) {
        for (const auto& candidate : {executable_path.parent_path().parent_path() / "share" / "llavon-ime" / "tables",
                                      executable_path.parent_path().parent_path() / "tables"}) {
            if (std::filesystem::is_directory(candidate)) return candidate;
        }
    }
    return cwd / "tables";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        imesvc::UnixServerOptions options;
        options.runtime.tables_dir = default_tables_dir(argv[0]);
        if (const char* value = env_value("LLAVON_IME_MODEL_PATH")) options.runtime.model_path = value;
        if (const char* value = env_value("LLAVON_IME_CONTEXT_LENGTH"))
            options.runtime.context_length = static_cast<std::uint32_t>(positive_number(value, "--context-length"));
        if (const char* value = env_value("LLAVON_IME_THREADS"))
            options.runtime.threads = static_cast<std::uint32_t>(positive_number(value, "--threads"));

        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            auto require_value = [&](const char* option) -> std::string_view {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + option);
                return argv[++i];
            };
            if (argument == "--help") {
                print_usage(argv[0]);
                return 0;
            } else if (argument == "--model") {
                options.runtime.model_path = require_value("--model");
            } else if (argument == "--tables") {
                options.runtime.tables_dir = require_value("--tables");
            } else if (argument == "--context-length") {
                options.runtime.context_length = static_cast<std::uint32_t>(positive_number(require_value("--context-length"), "--context-length"));
            } else if (argument == "--threads") {
                options.runtime.threads = static_cast<std::uint32_t>(positive_number(require_value("--threads"), "--threads"));
            } else if (argument == "--gpu-layers") {
                const auto value = require_value("--gpu-layers");
                if (value == "auto") {
                    options.runtime.gpu_layers = -2;
                } else {
                    options.runtime.gpu_layers = static_cast<int>(positive_number(value, "--gpu-layers"));
                }
            } else if (argument == "--max-sessions") {
                options.limits.max_sessions = positive_number(require_value("--max-sessions"), "--max-sessions");
            } else if (argument == "--max-idle-sessions") {
                options.limits.max_idle_sessions = positive_number(require_value("--max-idle-sessions"), "--max-idle-sessions");
            } else if (argument == "--max-concurrent-predictions") {
                options.limits.max_concurrent_predictions = positive_number(require_value("--max-concurrent-predictions"), "--max-concurrent-predictions");
            } else if (argument == "--idle-timeout") {
                options.limits.idle_timeout = std::chrono::seconds(positive_number(require_value("--idle-timeout"), "--idle-timeout"));
            } else if (argument == "--socket") {
                options.socket_path = std::filesystem::path(require_value("--socket"));
            } else if (argument == "--pid") {
                options.pid_path = std::filesystem::path(require_value("--pid"));
            } else if (argument == "--personal-learning") {
                options.personal_learning_enabled = true;
            } else if (argument == "--training-data-dir") {
                options.training_data_directory = std::filesystem::path(require_value("--training-data-dir"));
            } else if (argument == "--base-model-sha256") {
                options.runtime.base_model_sha256 = require_value("--base-model-sha256");
            } else if (argument == "--lora-training") {
                options.lora_training.enabled = true;
            } else if (argument == "--base-safetensors") {
                options.lora_training.base_safetensors = std::filesystem::path(require_value("--base-safetensors"));
            } else if (argument == "--trainer") {
                options.lora_training.trainer_executable = std::filesystem::path(require_value("--trainer"));
            } else if (argument == "--adapter-dir") {
                options.lora_training.adapter_directory = std::filesystem::path(require_value("--adapter-dir"));
            } else {
                throw std::runtime_error("unknown option: " + std::string(argument));
            }
        }

        if (options.runtime.context_length == 0 || options.runtime.context_length > 384) {
            throw std::runtime_error("--context-length must not exceed the native 384-token model window");
        }
        if (options.runtime.threads == 0) throw std::runtime_error("--threads must be positive");
        if (options.lora_training.enabled && options.lora_training.trainer_executable.empty()) {
            std::error_code error;
            const auto executable = std::filesystem::absolute(argv[0], error);
            if (!error) options.lora_training.trainer_executable = executable.parent_path() / "llavon-ime-trainer";
        }
        std::clog << "[SRV] IME Service starting\n";
        auto server = imesvc::create_server_strategy(std::move(options));
        std::clog << "[SRV] platform server: " << server->name() << '\n';
        return server->run();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()) == "--help") return 0;
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}

#endif
