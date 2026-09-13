#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llavon::ime::core {

class Logger;

enum class InferenceBackend : std::uint8_t {
    automatic,
    cpu,
    cuda,
    vulkan,
    metal,
};

enum class InferenceDeviceType : std::uint8_t {
    cpu,
    gpu,
    integrated_gpu,
};

struct InferenceDeviceSelection {
    InferenceBackend backend = InferenceBackend::automatic;
    std::string device_id;
};

struct InferenceDeviceInfo {
    InferenceBackend backend = InferenceBackend::cpu;
    InferenceDeviceType type = InferenceDeviceType::cpu;
    std::string device_id;
    std::string name;
    std::string description;
    std::uint64_t memory_free = 0;
    std::uint64_t memory_total = 0;
};

struct InferenceRuntimeInfo {
    InferenceDeviceInfo device;
    bool gpu_offload = false;
    bool fell_back_to_cpu = false;
};

struct CoreConfig {
    std::filesystem::path model_path;
    std::filesystem::path tables_dir;
    std::uint32_t context_length = 0;
    std::uint32_t threads = 8;
    int gpu_layers = -2;
    InferenceDeviceSelection inference_device;
    std::shared_ptr<Logger> logger;
};

// Enumerates devices from the inference backends that are available in the
// current process. This function does not load a model or read application
// settings.
std::vector<InferenceDeviceInfo> enumerate_inference_devices();

struct PaddingEntry {
    bool chosen = false;
    char32_t chosen_char = 0;
    std::u16string bopomofo;
};

struct Prediction {
    std::vector<std::pair<char32_t, float>> candidates;
};

class Session final {
public:
    ~Session();

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void ready();
    std::vector<Prediction> predict(
        const std::u16string& context,
        const std::vector<PaddingEntry>& padding);

private:
    class Impl;

    explicit Session(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class Core;
};

class Core final {
public:
    explicit Core(CoreConfig config);
    ~Core();

    Core(Core&&) noexcept;
    Core& operator=(Core&&) noexcept;

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    std::unique_ptr<Session> create_session() const;
    InferenceRuntimeInfo inference_runtime_info() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace llavon::ime::core
