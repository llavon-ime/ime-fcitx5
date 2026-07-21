#include "engine/llama_resources.hpp"

#include <ggml-backend.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "utils/runtime_paths.hpp"
#endif

namespace imesvc {
namespace {

struct BackendLifetime final {
    BackendLifetime() { llama_backend_init(); }
    ~BackendLifetime() { llama_backend_free(); }
};

void ensure_backend_initialized() {
    static BackendLifetime lifetime;
    (void)lifetime;
}

struct OffloadDevice {
    ggml_backend_dev_t device = nullptr;
    enum ggml_backend_dev_type type = GGML_BACKEND_DEVICE_TYPE_CPU;
    std::size_t memory_free = 0;
    std::size_t memory_total = 0;
};

const char* device_type_name(enum ggml_backend_dev_type type) {
    switch (type) {
        case GGML_BACKEND_DEVICE_TYPE_CPU: return "CPU";
        case GGML_BACKEND_DEVICE_TYPE_GPU: return "GPU";
        case GGML_BACKEND_DEVICE_TYPE_IGPU: return "IGPU";
        case GGML_BACKEND_DEVICE_TYPE_ACCEL: return "ACCEL";
        case GGML_BACKEND_DEVICE_TYPE_META: return "META";
        default: return "UNKNOWN";
    }
}

double mib(std::size_t bytes) {
    return static_cast<double>(bytes) / 1024.0 / 1024.0;
}

OffloadDevice select_gpu_device() {
    const std::size_t count = ggml_backend_dev_count();
    std::vector<OffloadDevice> candidates;
    for (std::size_t index = 0; index < count; ++index) {
        auto* device = ggml_backend_dev_get(index);
        ggml_backend_dev_props properties{};
        ggml_backend_dev_get_props(device, &properties);
        const char* name = properties.name ? properties.name : ggml_backend_dev_name(device);
        const char* description = properties.description ? properties.description : ggml_backend_dev_description(device);
        std::clog << "[SRV] llama device[" << index << "] type=" << device_type_name(properties.type)
                  << " name=" << (name ? name : "<unknown>")
                  << " desc=" << (description ? description : "<unknown>")
                  << " free_mib=" << std::fixed << std::setprecision(1) << mib(properties.memory_free)
                  << " total_mib=" << mib(properties.memory_total) << std::defaultfloat
                  << " id=" << (properties.device_id ? properties.device_id : "<unknown>") << '\n';
        if (properties.type == GGML_BACKEND_DEVICE_TYPE_GPU ||
            properties.type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            candidates.push_back({device, properties.type, properties.memory_free, properties.memory_total});
        }
    }
    if (candidates.empty()) return {};
    std::sort(candidates.begin(), candidates.end(), [](const OffloadDevice& left, const OffloadDevice& right) {
        if (left.type != right.type) return left.type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        if (left.memory_total != right.memory_total) return left.memory_total > right.memory_total;
        return left.memory_free > right.memory_free;
    });
    const auto selected = candidates.front();
    std::clog << "[SRV] llama selected offload device type=" << device_type_name(selected.type)
              << " name=" << (ggml_backend_dev_name(selected.device) ? ggml_backend_dev_name(selected.device) : "<unknown>")
              << " total_mib=" << std::fixed << std::setprecision(1) << mib(selected.memory_total)
              << std::defaultfloat << '\n';
    return selected;
}

}  // namespace

LlamaModelResources::LlamaModelResources(RuntimeConfig config)
    : config_(std::move(config)), tokenizer_(config_.tables_dir) {
    if (config_.model_path.empty()) throw std::invalid_argument("llama model path is empty");
    ensure_backend_initialized();
    auto parameters = llama_model_default_params();
    std::array<ggml_backend_dev_t, 2> offload_devices{};
    const auto offload_device = select_gpu_device();
    const bool supports_gpu_offload = llama_supports_gpu_offload();
    const bool wants_gpu = config_.gpu_layers != 0 &&
                           (config_.gpu_layers == -2 || config_.gpu_layers == -1 || config_.gpu_layers > 0);
    const bool use_gpu_offload = wants_gpu && offload_device.device != nullptr && supports_gpu_offload;
    if (use_gpu_offload) {
        offload_devices[0] = offload_device.device;
        parameters.devices = offload_devices.data();
        parameters.n_gpu_layers = config_.gpu_layers == -2 || config_.gpu_layers == -1 ? -1 : config_.gpu_layers;
        parameters.split_mode = LLAMA_SPLIT_MODE_NONE;
        parameters.main_gpu = 0;
    }
    const auto path = config_.model_path.string();
    std::clog << "[SRV] loading model: " << path << '\n'
              << "[SRV] llama gpu_offload=" << (supports_gpu_offload ? "supported" : "unavailable")
              << " gpu_layers=" << parameters.n_gpu_layers << " main_gpu=" << parameters.main_gpu << '\n'
              << "[SRV] llama offload=" << (use_gpu_offload ? "enabled" : "disabled") << '\n';
    model_.reset(llama_model_load_from_file(path.c_str(), parameters));
    if (!model_) throw std::runtime_error("Failed to load model: " + path);
    vocab_ = llama_model_get_vocab(model_.get());
    if (vocab_ == nullptr) throw std::runtime_error("loaded llama model has no vocabulary");
    std::clog << "[SRV] model loaded\n";
}

llama_model* LlamaModelResources::model() const noexcept { return model_.get(); }

const llama_vocab* LlamaModelResources::vocab() const noexcept { return vocab_; }

llama_context* LlamaModelResources::new_context(std::uint32_t context_length,
                                                std::uint32_t batch_size) const {
    auto parameters = llama_context_default_params();
    parameters.n_ctx = context_length == 0 ? config_.context_length : context_length;
    if (batch_size != 0) {
        parameters.n_batch = batch_size;
        parameters.n_ubatch = batch_size;
    }
    parameters.n_threads = static_cast<std::int32_t>(config_.threads);
    parameters.n_threads_batch = static_cast<std::int32_t>(config_.threads);
    auto* context = llama_init_from_model(model_.get(), parameters);
    if (context == nullptr) throw std::runtime_error("Failed to create llama context");
    std::clog << "[SRV] context created threads=" << config_.threads << '\n';
    return context;
}

const Tokenizer& LlamaModelResources::tokenizer() const noexcept { return tokenizer_; }

#ifdef _WIN32
std::shared_ptr<LlamaModelResources> legacy_llama_resources() {
    static auto resources = [] {
        RuntimeConfig config;
        config.model_path = RuntimePaths::model_path();
        config.tables_dir = RuntimePaths::tables_dir();
        config.context_length = RuntimePaths::context_length() == 0 ? 384 : RuntimePaths::context_length();
        config.threads = RuntimePaths::threads();
        config.gpu_layers = RuntimePaths::gpu_layers();
        return std::make_shared<LlamaModelResources>(std::move(config));
    }();
    return resources;
}
#endif

}  // namespace imesvc
