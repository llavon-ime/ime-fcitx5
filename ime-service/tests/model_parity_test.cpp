#include "ml/gguf_lora_writer.hpp"
#include "ml/llama_model.hpp"
#include "ml/safetensors_reader.hpp"
#include "ml/training_tokenizer.hpp"

#include <llama.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

struct ModelDeleter {
    void operator()(llama_model* model) const { llama_model_free(model); }
};
struct ContextDeleter {
    void operator()(llama_context* context) const { llama_free(context); }
};
struct AdapterDeleter {
    void operator()(llama_adapter_lora* adapter) const { llama_adapter_lora_free(adapter); }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: model_parity_test MODEL_SAFETENSORS SHA256 F16_GGUF TABLES\n";
        return EXIT_FAILURE;
    }
    try {
        torch::set_num_threads(4);
        torch::set_num_interop_threads(1);
        torch::manual_seed(20260713);
        imesvc::ml::LlamaForCausalLm torch_model;
        imesvc::ml::SafetensorsReader().load_fixed_llama(
            argv[1], argv[2], torch_model->safetensors_parameters());
        torch_model->eval();

        imesvc::ml::TrainingTokenizer tokenizer(argv[4]);
        const std::vector<std::int64_t> token_ids{
            tokenizer.special("<BOS>"), tokenizer.character_token("你"), tokenizer.bopomofo_token("ㄏㄠˇ")};
        torch::NoGradGuard no_grad;
        const auto base_logits = torch_model->forward(torch::tensor(token_ids, torch::kInt64).unsqueeze(0)).contiguous();
        for (const auto& [name, tensor] : torch_model->adapter_tensors()) {
            if (name.ends_with(".lora_b")) tensor.normal_(0.0, 0.001);
        }
        const auto torch_logits = torch_model->forward(torch::tensor(token_ids, torch::kInt64).unsqueeze(0)).contiguous();
        require(torch_logits.sizes() == torch::IntArrayRef({1, 3, imesvc::ml::LlamaModelConfig::kVocabularySize}),
                "unexpected LibTorch model output shape");
        require(torch::isfinite(torch_logits).all().item<bool>(), "LibTorch model produced non-finite logits");
        require((torch_logits - base_logits).abs().max().item<double>() > 1e-4,
                "model parity fixture did not activate a nonzero LoRA update");

        llama_backend_init();
        auto model_parameters = llama_model_default_params();
        model_parameters.n_gpu_layers = 0;
        std::unique_ptr<llama_model, ModelDeleter> llama_model(
            llama_model_load_from_file(argv[3], model_parameters));
        require(llama_model != nullptr, "llama.cpp could not load the F16 fixture");
        require(llama_vocab_n_tokens(llama_model_get_vocab(llama_model.get())) ==
                    imesvc::ml::LlamaModelConfig::kVocabularySize,
                "llama.cpp fixture vocabulary mismatch");

        auto context_parameters = llama_context_default_params();
        context_parameters.n_ctx = imesvc::ml::LlamaModelConfig::kContextLength;
        context_parameters.n_batch = static_cast<std::uint32_t>(token_ids.size());
        context_parameters.n_ubatch = static_cast<std::uint32_t>(token_ids.size());
        context_parameters.n_threads = 4;
        context_parameters.n_threads_batch = 4;
        context_parameters.no_perf = true;
        std::unique_ptr<llama_context, ContextDeleter> context(
            llama_init_from_model(llama_model.get(), context_parameters));
        require(context != nullptr, "llama.cpp could not create a context");

        const auto adapter_path = std::filesystem::temp_directory_path() / "llavon-ime-parity-adapter.gguf";
        imesvc::ml::GgufLoraWriter::write_f32_atomic(adapter_path, torch_model->gguf_adapter_tensors(), {});
        std::unique_ptr<llama_adapter_lora, AdapterDeleter> adapter(
            llama_adapter_lora_init(llama_model.get(), adapter_path.c_str()));
        require(adapter != nullptr, "llama.cpp rejected the generated GGUF adapter");
        auto* adapter_pointer = adapter.get();
        float adapter_scale = 1.0F;
        require(llama_set_adapters_lora(context.get(), &adapter_pointer, 1, &adapter_scale) == 0,
                "llama.cpp could not apply the generated GGUF adapter");

        auto batch = llama_batch_init(static_cast<int32_t>(token_ids.size()), 0, 1);
        batch.n_tokens = static_cast<int32_t>(token_ids.size());
        for (std::size_t index = 0; index < token_ids.size(); ++index) {
            batch.token[index] = static_cast<llama_token>(token_ids[index]);
            batch.pos[index] = static_cast<llama_pos>(index);
            batch.n_seq_id[index] = 1;
            batch.seq_id[index][0] = 0;
            batch.logits[index] = 1;
        }
        const auto decode_result = llama_decode(context.get(), batch);
        llama_batch_free(batch);
        require(decode_result == 0, "llama.cpp decode failed");

        double maximum_error = 0.0;
        for (std::size_t position = 0; position < token_ids.size(); ++position) {
            const float* raw_logits = llama_get_logits_ith(context.get(), static_cast<int32_t>(position));
            require(raw_logits != nullptr, "llama.cpp did not return all requested logits");
            const auto llama_logits = torch::from_blob(
                const_cast<float*>(raw_logits), {imesvc::ml::LlamaModelConfig::kVocabularySize}, torch::kFloat32);
            const auto error = (torch_logits[0][static_cast<std::int64_t>(position)] - llama_logits)
                                   .abs().max().item<double>();
            maximum_error = std::max(maximum_error, error);
        }
        std::error_code cleanup_error;
        std::filesystem::remove(adapter_path, cleanup_error);
        std::cout << "maximum F32-LibTorch vs F16-GGUF logit error: " << maximum_error << '\n';
        require(maximum_error < 0.1, "LibTorch and llama.cpp logits exceed the F16 parity tolerance");
        llama_backend_free();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "model_parity_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
