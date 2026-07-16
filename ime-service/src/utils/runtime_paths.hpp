#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace imesvc {

class RuntimePaths {
    struct Paths {
        std::filesystem::path model_path;
        std::filesystem::path tables_dir;
        std::uint32_t context_length = 0;
        std::uint32_t threads = 8;
        int gpu_layers = -2;
    };

public:
    static void configure(std::filesystem::path model_path, std::filesystem::path tables_dir,
                          std::uint32_t context_length = 0, std::uint32_t threads = 8, int gpu_layers = -2) {
        model_path = normalize(std::move(model_path));
        tables_dir = normalize(std::move(tables_dir));

        if (!std::filesystem::is_regular_file(model_path)) {
            throw std::runtime_error("model file not found: " + model_path.string());
        }

        if (!std::filesystem::is_directory(tables_dir)) {
            throw std::runtime_error("tables directory not found: " + tables_dir.string());
        }

        require_file(tables_dir / "tokens" / "chars.json");
        require_file(tables_dir / "tokens" / "latin.json");
        require_file(tables_dir / "tokens" / "special_tokens.json");
        require_file(tables_dir / "tokens" / "bpmf.json");
        require_file(tables_dir / "bopomofo_char.json");

        if (threads == 0) throw std::runtime_error("threads must be positive");
        storage() = Paths{std::move(model_path), std::move(tables_dir), context_length, threads, gpu_layers};
    }

    static const std::filesystem::path& model_path() {
        return configured().model_path;
    }

    static std::filesystem::path token_table_path(const char* filename) {
        return configured().tables_dir / "tokens" / filename;
    }

    static std::filesystem::path bopomofo_table_path() {
        return configured().tables_dir / "bopomofo_char.json";
    }

    static std::uint32_t context_length() { return configured().context_length; }
    static std::uint32_t threads() { return configured().threads; }
    static int gpu_layers() { return configured().gpu_layers; }

private:
    static std::optional<Paths>& storage() {
        static std::optional<Paths> paths;
        return paths;
    }

    static const Paths& configured() {
        const auto& paths = storage();
        if (!paths) {
            throw std::runtime_error("runtime paths were not configured");
        }
        return *paths;
    }

    static std::filesystem::path normalize(std::filesystem::path path) {
        if (path.is_relative()) {
            path = std::filesystem::absolute(path);
        }
        return path.lexically_normal();
    }

    static void require_file(const std::filesystem::path& path) {
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("required table file not found: " + path.string());
        }
    }
};

}  // namespace imesvc
