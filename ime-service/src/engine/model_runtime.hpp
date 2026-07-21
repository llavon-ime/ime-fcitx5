#pragma once

#include "../pipe/protocol.hpp"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef IMESVC_HAS_LLAMA
#define IMESVC_HAS_LLAMA 0
#endif

namespace imesvc {

#if IMESVC_HAS_LLAMA
class LlamaModelResources;
#endif

struct RuntimeConfig {
    std::filesystem::path model_path;
    std::filesystem::path tables_dir;
    std::string base_model_sha256;
    std::uint32_t context_length = 384;
    std::uint32_t threads = 8;
    int gpu_layers = -2;  // -2 means auto; -1 means all layers when a backend exists.
};

struct AdapterGeneration {
    std::uint64_t revision = 0;
    std::string version;
    std::filesystem::path path;
    std::string sha256;
};

struct ModelRuntimeStatus {
    bool tables_loaded = false;
    bool inference_model_loaded = false;
    bool adapter_loaded = false;
    std::string active_adapter_version;
    std::string error;
};

class SharedModelRuntime final {
public:
    explicit SharedModelRuntime(RuntimeConfig config);

    SharedModelRuntime(const SharedModelRuntime&) = delete;
    SharedModelRuntime& operator=(const SharedModelRuntime&) = delete;

    const RuntimeConfig& config() const noexcept;
    void validate_configuration() const;
    void ensure_loaded() const;
    bool loaded() const noexcept;
    std::string load_error() const;
    [[nodiscard]] ModelRuntimeStatus status() const;
    void record_inference_loaded(bool adapter_loaded) noexcept;
    void record_inference_failure(std::string error) noexcept;
#if IMESVC_HAS_LLAMA
    [[nodiscard]] std::shared_ptr<LlamaModelResources> llama_resources() const;
#endif

    std::vector<char32_t> lookup(std::u16string_view bopomofo) const;
    [[nodiscard]] std::shared_ptr<const AdapterGeneration> adapter_generation() const;
    [[nodiscard]] std::string active_adapter_version() const;
    void activate_adapter(std::string version, std::filesystem::path path, std::string sha256);
    void clear_active_adapter() noexcept;
    void set_adapter_failure_handler(std::function<void(std::string)> handler);
    void reject_active_adapter(const std::string& version);

private:
    void load_tables() const;
    static std::u16string utf8_to_utf16(const std::string& value);
    static char32_t first_codepoint(const std::string& value);

    RuntimeConfig config_;
    mutable std::once_flag load_once_;
    mutable std::mutex state_mutex_;
    mutable bool loaded_ = false;
    mutable std::string load_error_;
    bool inference_model_loaded_ = false;
    bool adapter_loaded_ = false;
    std::string inference_load_error_;
    mutable std::unordered_map<std::u16string, std::vector<char32_t>> bopomofo_table_;
    std::shared_ptr<const AdapterGeneration> adapter_generation_;
    std::uint64_t next_adapter_revision_ = 1;
    std::function<void(std::string)> adapter_failure_handler_;
#if IMESVC_HAS_LLAMA
    mutable std::once_flag llama_resources_once_;
    mutable std::shared_ptr<LlamaModelResources> llama_resources_;
#endif
};

}  // namespace imesvc
