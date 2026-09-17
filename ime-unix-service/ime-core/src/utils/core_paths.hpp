#pragma once

#include <ime-core/core.hpp>

#include <filesystem>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace llavon::ime::core::internal {

class CorePaths {
public:
    CorePaths(std::filesystem::path model_path, std::filesystem::path tables_dir,
              std::uint32_t context_length = 0, std::uint32_t threads = 8,
              int gpu_layers = -2,
              InferenceDeviceSelection inference_device = {})
        : model_path_(normalize(std::move(model_path))),
          tables_dir_(normalize(std::move(tables_dir))),
          context_length_(context_length),
          threads_(threads),
          gpu_layers_(gpu_layers),
          inference_device_(std::move(inference_device)) {

        if (!std::filesystem::is_regular_file(model_path_)) {
            throw std::runtime_error("model file not found: " + model_path_.string());
        }

        if (!std::filesystem::is_directory(tables_dir_)) {
            throw std::runtime_error("tables directory not found: " + tables_dir_.string());
        }

        require_file(tables_dir_ / "tokens" / "chars.json");
        require_file(tables_dir_ / "tokens" / "latin.json");
        require_file(tables_dir_ / "tokens" / "special_tokens.json");
        require_file(tables_dir_ / "tokens" / "bpmf.json");
        require_file(tables_dir_ / "bopomofo_char.json");

        if (threads_ == 0) throw std::runtime_error("threads must be positive");
    }

    const std::filesystem::path& model_path() const noexcept { return model_path_; }

    std::filesystem::path token_table_path(const char* filename) const {
        return tables_dir_ / "tokens" / filename;
    }

    std::filesystem::path bopomofo_table_path() const {
        return tables_dir_ / "bopomofo_char.json";
    }

    std::uint32_t context_length() const noexcept { return context_length_; }
    std::uint32_t threads() const noexcept { return threads_; }
    int gpu_layers() const noexcept { return gpu_layers_; }
    const InferenceDeviceSelection& inference_device() const noexcept {
        return inference_device_;
    }

private:
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

    std::filesystem::path model_path_;
    std::filesystem::path tables_dir_;
    std::uint32_t context_length_ = 0;
    std::uint32_t threads_ = 8;
    int gpu_layers_ = -2;
    InferenceDeviceSelection inference_device_;
};

}  // namespace llavon::ime::core::internal
