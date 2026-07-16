#include "ml/gguf_lora_writer.hpp"

#include <ggml.h>
#include <gguf.h>
#include <llama.h>

#include <stdexcept>
#include <vector>

namespace imesvc::ml {

void GgufLoraWriter::write_f32_atomic(const std::filesystem::path& output,
                                      const std::unordered_map<std::string, torch::Tensor>& tensors,
                                      const LoraConfig& config) {
    config.validate();
    if (tensors.empty()) throw std::invalid_argument("cannot write an empty LoRA adapter");
    const auto temporary = output.string() + ".tmp";
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
        std::error_code error; std::filesystem::rename(temporary, output, error);
        if (error) throw std::runtime_error("publish GGUF adapter failed: " + error.message());
    } catch (...) { gguf_free(gguf); ggml_free(tensor_context); throw; }
    gguf_free(gguf); ggml_free(tensor_context);
}

void GgufLoraWriter::validate_loadable(const std::filesystem::path& adapter, llama_model* model) {
    if (model == nullptr) throw std::invalid_argument("a loaded F32 llama.cpp model is required for adapter validation");
    llama_adapter_lora* loaded = llama_adapter_lora_init(model, adapter.c_str());
    if (loaded == nullptr) throw std::runtime_error("llama.cpp rejected the GGUF LoRA adapter");
    llama_adapter_lora_free(loaded);
}

}  // namespace imesvc::ml
