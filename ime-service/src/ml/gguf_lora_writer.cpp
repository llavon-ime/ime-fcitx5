#include "ml/gguf_lora_writer.hpp"

#include <ggml.h>
#include <gguf.h>
#include <llama.h>

#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace imesvc::ml {
namespace {

void flush_path(const std::filesystem::path& path) {
#ifndef _WIN32
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0 || ::fsync(descriptor) != 0) {
        if (descriptor >= 0) (void)::close(descriptor);
        throw std::runtime_error("flush GGUF adapter artifact failed");
    }
    if (::close(descriptor) != 0) throw std::runtime_error("close GGUF adapter artifact failed");
#else
    (void)path;
#endif
}

void prepare_temporary_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return;
    if (error) throw std::runtime_error("inspect temporary GGUF adapter failed: " + error.message());
    if (!std::filesystem::exists(status)) return;
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("refusing unsafe temporary GGUF adapter path");
    }
    if (!std::filesystem::remove(path, error) || error) {
        throw std::runtime_error("remove stale temporary GGUF adapter failed");
    }
}

}  // namespace

void GgufLoraWriter::write_f32_atomic(const std::filesystem::path& output,
                                      const std::unordered_map<std::string, torch::Tensor>& tensors,
                                      const LoraConfig& config) {
    config.validate();
    if (tensors.empty()) throw std::invalid_argument("cannot write an empty LoRA adapter");
    const std::filesystem::path temporary = output.string() + ".tmp";
    prepare_temporary_file(temporary);
    ggml_init_params parameters{}; parameters.mem_size = 4U * 1024U * 1024U; parameters.no_alloc = true;
    auto* tensor_context = ggml_init(parameters);
    auto* gguf = gguf_init_empty();
    if (!tensor_context || !gguf) { if (gguf) gguf_free(gguf); if (tensor_context) ggml_free(tensor_context); throw std::runtime_error("initialize GGUF writer failed"); }
    try {
        gguf_set_val_str(gguf, "general.type", "adapter"); gguf_set_val_str(gguf, "general.architecture", "llama");
        gguf_set_val_str(gguf, "adapter.type", "lora"); gguf_set_val_f32(gguf, "adapter.lora.alpha", static_cast<float>(config.alpha));
        std::vector<torch::Tensor> retained;
        for (const auto& [name, tensor] : tensors) {
            if (!tensor.defined() || tensor.dim() != 2) throw std::runtime_error("invalid LoRA tensor: " + name);
            auto value = tensor.detach().to(torch::TensorOptions().device(torch::kCPU).dtype(torch::kFloat32)).contiguous();
            retained.push_back(value);
            auto* ggml_tensor = ggml_new_tensor_2d(tensor_context, GGML_TYPE_F32, value.size(1), value.size(0));
            ggml_set_name(ggml_tensor, name.c_str());
            gguf_add_tensor(gguf, ggml_tensor);
            gguf_set_tensor_data(gguf, name.c_str(), value.data_ptr());
        }
        if (!gguf_write_to_file(gguf, temporary.c_str(), false)) throw std::runtime_error("write GGUF adapter failed");
        flush_path(temporary);
        std::error_code error;
        std::filesystem::rename(temporary, output, error);
        if (error) throw std::runtime_error("publish GGUF adapter failed: " + error.message());
        if (!output.parent_path().empty()) flush_path(output.parent_path());
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        gguf_free(gguf);
        ggml_free(tensor_context);
        throw;
    }
    gguf_free(gguf); ggml_free(tensor_context);
}

void GgufLoraWriter::validate_loadable(const std::filesystem::path& adapter, llama_model* model) {
    if (model == nullptr) throw std::invalid_argument("a loaded F32 llama.cpp model is required for adapter validation");
    llama_adapter_lora* loaded = llama_adapter_lora_init(model, adapter.c_str());
    if (loaded == nullptr) throw std::runtime_error("llama.cpp rejected the GGUF LoRA adapter");
    llama_adapter_lora_free(loaded);
}

}  // namespace imesvc::ml
