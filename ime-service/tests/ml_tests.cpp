#include "ml/checkpoint.hpp"
#include "engine/tokenizer.hpp"
#include "ml/gguf_lora_writer.hpp"
#include "ml/llama_model.hpp"
#include "ml/lora_linear.hpp"
#include "ml/rms_norm.hpp"
#include "ml/rotary_embedding.hpp"
#include "ml/safetensors_reader.hpp"
#include "ml/training_dataset.hpp"

#include <gguf.h>
#include <ATen/CPUGeneratorImpl.h>
#include <nlohmann/json.hpp>
#include <utf8/cpp20.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_directory() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("llavon-ime-ml-tests-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path write_safetensors_fixture(const std::filesystem::path& directory,
                                                const std::string& name,
                                                const std::string& header,
                                                const std::vector<std::uint8_t>& data = {}) {
    const auto path = directory / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const auto header_size = static_cast<std::uint64_t>(header.size());
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.put(static_cast<char>(header_size >> shift));
    }
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!output) throw std::runtime_error("write malformed safetensors fixture failed");
    return path;
}

void require_safetensors_failure(const std::filesystem::path& path,
                                 const std::unordered_map<std::string, torch::Tensor>& destinations,
                                 std::string_view expected,
                                 imesvc::ml::SafetensorsLimits limits = {}) {
    const auto sha256 = imesvc::ml::SafetensorsReader::sha256_file(path);
    try {
        imesvc::ml::SafetensorsReader(limits).load_fixed_llama(path, sha256, destinations);
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                "safetensors rejection used the wrong validation path");
        return;
    }
    throw std::runtime_error("malformed safetensors fixture was accepted");
}

void test_rms_norm_and_rope() {
    imesvc::ml::RmsNorm norm(4, 1e-5);
    const auto input = torch::tensor({{1.0F, 2.0F, 3.0F, 4.0F}});
    const auto expected = input * torch::rsqrt(input.pow(2).mean(-1, true) + 1e-5);
    require(torch::allclose(norm->forward(input), expected, 1e-6, 1e-6), "RMSNorm numerical mismatch");

    imesvc::ml::RotaryEmbedding rotary(4, 10000.0);
    const auto query = torch::randn({1, 2, 3, 4});
    const auto key = torch::randn({1, 2, 3, 4});
    const auto [rotated_query, rotated_key] = rotary.apply(query, key);
    require(torch::allclose(rotated_query.select(2, 0), query.select(2, 0), 1e-6, 1e-6),
            "RoPE position zero must be unchanged");
    require(torch::allclose(rotated_query.pow(2).sum(-1), query.pow(2).sum(-1), 1e-5, 1e-5),
            "RoPE must preserve query norms");
    require(torch::allclose(rotated_key.pow(2).sum(-1), key.pow(2).sum(-1), 1e-5, 1e-5),
            "RoPE must preserve key norms");
}

void test_lora_gradients_and_math() {
    imesvc::ml::LoraConfig config;
    config.rank = 2;
    config.alpha = 4.0;
    config.dropout = 0.0;
    imesvc::ml::LoraLinear layer(3, 4, config);
    layer->weight.copy_(torch::randn_like(layer->weight));
    const auto input = torch::randn({5, 3});
    const auto base_weight = layer->weight.detach().clone();
    const auto initial = torch::nn::functional::linear(input, layer->weight);
    require(torch::allclose(layer->forward(input), initial), "zero LoRA B must preserve base output");

    torch::optim::AdamW optimizer(layer->adapter_parameters(), torch::optim::AdamWOptions(1e-2));
    layer->forward(input).sum().backward();
    layer->assert_base_is_frozen();
    require(!layer->weight.grad().defined(), "frozen LoRA base received a gradient");
    const auto before_b = layer->lora_b.detach().clone();
    optimizer.step();
    require(torch::equal(layer->weight, base_weight), "optimizer changed frozen LoRA base");
    require(!torch::equal(layer->lora_b, before_b), "optimizer did not update LoRA B");

    {
        torch::NoGradGuard guard;
        layer->lora_a.copy_(torch::randn_like(layer->lora_a));
        layer->lora_b.copy_(torch::randn_like(layer->lora_b));
    }
    layer->eval();
    const auto merged_weight = layer->weight + 2.0 * torch::matmul(layer->lora_b, layer->lora_a);
    require(torch::allclose(layer->forward(input), torch::nn::functional::linear(input, merged_weight), 1e-5, 1e-5),
            "dynamic LoRA output does not match merged weights");
}

void test_gradient_accumulation_equivalence() {
    imesvc::ml::LoraConfig config;
    config.rank = 2;
    config.alpha = 4.0;
    config.dropout = 0.0;
    imesvc::ml::LoraLinear batched(3, 4, config);
    imesvc::ml::LoraLinear accumulated(3, 4, config);
    {
        torch::NoGradGuard guard;
        batched->weight.normal_(0.0, 0.1);
        batched->lora_a.normal_(0.0, 0.1);
        batched->lora_b.normal_(0.0, 0.1);
        accumulated->weight.copy_(batched->weight);
        accumulated->lora_a.copy_(batched->lora_a);
        accumulated->lora_b.copy_(batched->lora_b);
    }
    const auto inputs = torch::randn({2, 3});
    const auto targets = torch::randn({2, 4});
    torch::optim::SGD batched_optimizer(batched->adapter_parameters(), torch::optim::SGDOptions(1e-2));
    torch::optim::SGD accumulated_optimizer(accumulated->adapter_parameters(), torch::optim::SGDOptions(1e-2));

    torch::mse_loss(batched->forward(inputs), targets).backward();
    batched_optimizer.step();
    for (std::int64_t index = 0; index < inputs.size(0); ++index) {
        torch::mse_loss(accumulated->forward(inputs[index].unsqueeze(0)), targets[index].unsqueeze(0)).backward();
    }
    for (auto& parameter : accumulated->adapter_parameters()) parameter.grad().div_(inputs.size(0));
    accumulated_optimizer.step();

    require(torch::allclose(batched->lora_a, accumulated->lora_a, 1e-6, 1e-6) &&
                torch::allclose(batched->lora_b, accumulated->lora_b, 1e-6, 1e-6),
            "gradient accumulation differs from an equivalent batch");
}

void test_attention_swiglu_and_causality() {
    imesvc::ml::LlamaModelConfig model;
    model.hidden_size = 8;
    model.attention_heads = 2;
    model.kv_heads = 2;
    model.intermediate_size = 16;
    model.context_length = 8;
    imesvc::ml::LoraConfig lora;
    lora.rank = 2;
    lora.alpha = 4.0;
    lora.dropout = 0.0;

    imesvc::ml::CausalSelfAttention attention(model, lora);
    imesvc::ml::LlamaMlp mlp(model, lora);
    {
        torch::NoGradGuard guard;
        for (const auto& projection : {attention->q_proj, attention->k_proj, attention->v_proj, attention->o_proj}) {
            projection->weight.normal_(0.0, 0.1);
        }
        for (const auto& projection : {mlp->gate_proj, mlp->up_proj, mlp->down_proj}) {
            projection->weight.normal_(0.0, 0.1);
        }
    }
    attention->eval();
    mlp->eval();
    const auto input = torch::randn({1, 4, model.hidden_size});
    auto changed_future = input.clone();
    changed_future.slice(1, 2).add_(100.0);
    const auto original_attention = attention->forward(input);
    const auto changed_attention = attention->forward(changed_future);
    require(original_attention.sizes() == input.sizes() && torch::isfinite(original_attention).all().item<bool>(),
            "attention output shape or values are invalid");
    require(torch::allclose(original_attention.slice(1, 0, 2), changed_attention.slice(1, 0, 2), 1e-5, 1e-5),
            "causal attention allowed future tokens to alter earlier outputs");

    const auto gate = torch::nn::functional::linear(input, mlp->gate_proj->weight);
    const auto up = torch::nn::functional::linear(input, mlp->up_proj->weight);
    const auto expected = torch::nn::functional::linear(torch::silu(gate) * up, mlp->down_proj->weight);
    require(torch::allclose(mlp->forward(input), expected, 1e-6, 1e-6), "SwiGLU numerical mismatch");
}

void test_tokenizer_parity() {
    imesvc::Tokenizer runtime_tokenizer(IMESVC_TEST_TABLES_DIRECTORY);
    imesvc::ml::TrainingTokenizer training_tokenizer(IMESVC_TEST_TABLES_DIRECTORY);
    for (const std::u16string context : {u"ABC-12 你好", u"未知🙂abc", u"--- 你"}) {
        const auto runtime = runtime_tokenizer.encode_context(context);
        const auto training = training_tokenizer.encode_context(utf8::utf16to8(context));
        require(std::vector<std::int64_t>(runtime.begin(), runtime.end()) == training,
                "training and runtime context tokenizers disagree");
    }
}

void test_safetensors_rejections() {
    const auto directory = temporary_directory();
    try {
        const std::vector<std::uint8_t> four_bytes(4);
        const auto tensor = torch::zeros({1}, torch::kFloat32);
        const std::unordered_map<std::string, torch::Tensor> one{{"tensor", tensor}};
        const auto metadata = [](std::string dtype, nlohmann::json shape, nlohmann::json offsets) {
            return nlohmann::json{{"tensor", {{"dtype", std::move(dtype)}, {"shape", std::move(shape)},
                                                   {"data_offsets", std::move(offsets)}}}}.dump();
        };

        const auto sha_mismatch = write_safetensors_fixture(directory, "sha-mismatch.safetensors", "{}");
        try {
            imesvc::ml::SafetensorsReader().load_fixed_llama(sha_mismatch, std::string(64, '0'), {});
            throw std::runtime_error("safetensors SHA mismatch was accepted");
        } catch (const std::exception& error) {
            require(std::string_view(error.what()).find("SHA256 mismatch") != std::string_view::npos,
                    "safetensors SHA mismatch used the wrong validation path");
        }

        const auto oversized = write_safetensors_fixture(directory, "oversized-header.safetensors", std::string(64, ' '));
        auto limits = imesvc::ml::SafetensorsLimits{};
        limits.max_header_bytes = 16;
        require_safetensors_failure(oversized, {}, "header length", limits);

        const std::string duplicate_header =
            R"({"tensor":{"dtype":"F32","shape":[1],"data_offsets":[0,4]},"tensor":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})";
        require_safetensors_failure(
            write_safetensors_fixture(directory, "duplicate.safetensors", duplicate_header, four_bytes),
            one, "duplicate safetensors JSON key");

        const auto unknown_header = nlohmann::json{{"unknown", {{"dtype", "F32"}, {"shape", {1}},
                                                                  {"data_offsets", {0, 4}}}}}.dump();
        require_safetensors_failure(
            write_safetensors_fixture(directory, "unknown.safetensors", unknown_header, four_bytes),
            {}, "unknown safetensors tensor");
        require_safetensors_failure(write_safetensors_fixture(directory, "missing.safetensors", "{}"), one,
                                    "missing safetensors tensor");
        require_safetensors_failure(
            write_safetensors_fixture(directory, "dtype.safetensors", metadata("F16", {1}, {0, 4}), four_bytes),
            one, "unsupported safetensors tensor");
        require_safetensors_failure(
            write_safetensors_fixture(directory, "shape.safetensors", metadata("F32", nlohmann::json::array(), {0, 4}), four_bytes),
            one, "invalid safetensors shape");
        require_safetensors_failure(
            write_safetensors_fixture(directory, "truncated.safetensors", metadata("F32", {1}, {0, 8}), four_bytes),
            one, "invalid safetensors byte range");

        const std::unordered_map<std::string, torch::Tensor> two{{"a", tensor}, {"b", tensor}};
        const auto overlap_header = nlohmann::json{
            {"a", {{"dtype", "F32"}, {"shape", {1}}, {"data_offsets", {0, 4}}}},
            {"b", {{"dtype", "F32"}, {"shape", {1}}, {"data_offsets", {0, 4}}}}}.dump();
        require_safetensors_failure(
            write_safetensors_fixture(directory, "overlap.safetensors", overlap_header, four_bytes),
            two, "overlapping safetensors ranges");
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        throw;
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void test_causal_loss_and_dataset() {
    const auto logits = torch::tensor({{{2.0F, 0.0F, -1.0F}, {0.0F, 3.0F, 1.0F}, {1.0F, 1.0F, 1.0F}}});
    const auto labels = torch::tensor(
        std::vector<std::int64_t>{imesvc::ml::kIgnoreLabel, 1, 2}, torch::kInt64).unsqueeze(0);
    const auto expected = torch::nn::functional::cross_entropy(
        logits.slice(1, 0, 2).reshape({-1, 3}), torch::tensor({1, 2}, torch::kInt64));
    require(torch::allclose(imesvc::ml::causal_loss(logits, labels), expected), "causal loss shift mismatch");

    imesvc::ml::TrainingTokenizer tokenizer(IMESVC_TEST_TABLES_DIRECTORY);
    imesvc::training::DatasetSample sample;
    for (int index = 0; index < 500; ++index) sample.left_context += "你";
    sample.bopomofo_sequence = "ㄋㄧˇ";
    sample.committed_characters = "你";
    const auto example = imesvc::ml::make_training_example(sample, tokenizer);
    require(example.tokens.size(1) <= imesvc::ml::LlamaModelConfig::kContextLength,
            "training example exceeded native context length");
    require(example.labels.ne(imesvc::ml::kIgnoreLabel).sum().item<std::int64_t>() == 1,
            "training labels must include only committed targets");
}

void test_checkpoint_and_gguf() {
    const auto directory = temporary_directory();
    try {
        imesvc::ml::LoraConfig config;
        config.rank = 2;
        config.alpha = 4;
        config.dropout = 0;
        imesvc::ml::LoraLinear layer(3, 4, config);
        torch::optim::AdamW optimizer(layer->adapter_parameters(), torch::optim::AdamWOptions(1e-2));
        const auto first_input = torch::randn({2, 3});
        const auto second_input = torch::randn({2, 3});
        layer->forward(first_input).pow(2).mean().backward();
        optimizer.step();
        optimizer.zero_grad();
        const auto checkpoint_a = layer->lora_a.detach().clone();
        const auto checkpoint_b = layer->lora_b.detach().clone();
        const auto base_weight = layer->weight.detach().clone();

        imesvc::ml::CheckpointState state;
        state.adapter_tensors = {{"lora_a", layer->lora_a}, {"lora_b", layer->lora_b}};
        state.rng_state = at::detail::getDefaultCPUGenerator().get_state();
        state.step = 7;
        state.scheduler_step = 7;
        state.epoch = 1;
        state.dataset_cursor = 9;
        state.seed = 42;
        state.dataset_snapshot_id = "snapshot";
        state.base_model_sha256 = "base-sha";
        state.runtime_model_sha256 = "runtime-sha";
        state.tokenizer_sha256 = "tokenizer-sha";
        state.candidate_map_sha256 = "candidate-sha";
        state.training_code_version = "code-v1";
        state.lora_rank = 2;
        state.lora_alpha = 4.0;
        state.lora_dropout = 0.1;
        state.validation_loss_before = 1.25;
        state.accumulated_training_loss = 3.5;
        state.accumulated_training_weight = 2.0;
        imesvc::ml::Checkpoint::save_atomic(directory / "checkpoint", state, optimizer);

        imesvc::ml::LoraLinear restored_layer(3, 4, config);
        torch::optim::AdamW restored_optimizer(restored_layer->adapter_parameters(), torch::optim::AdamWOptions(1e-2));
        const auto restored = imesvc::ml::Checkpoint::load(directory / "checkpoint", restored_optimizer);
        require(restored.step == 7 && restored.scheduler_step == 7 && restored.epoch == 1 && restored.dataset_cursor == 9 &&
                    restored.seed == 42 && restored.accumulated_training_loss == 3.5 &&
                    restored.accumulated_training_weight == 2.0 &&
                    restored.dataset_snapshot_id == "snapshot" && restored.base_model_sha256 == "base-sha" &&
                    restored.runtime_model_sha256 == "runtime-sha" && restored.tokenizer_sha256 == "tokenizer-sha" &&
                    restored.candidate_map_sha256 == "candidate-sha" && restored.training_code_version == "code-v1" &&
                    restored.lora_rank == 2 && restored.lora_alpha == 4.0 && restored.lora_dropout == 0.1 &&
                    std::abs(restored.validation_loss_before - 1.25) < 1e-12 &&
                    restored.rng_state.defined(),
                "checkpoint metadata round-trip failed");
        require(torch::equal(restored.adapter_tensors.at("lora_a"), layer->lora_a) &&
                    torch::equal(restored.adapter_tensors.at("lora_b"), layer->lora_b),
                "checkpoint tensor round-trip failed");
        {
            torch::NoGradGuard guard;
            restored_layer->weight.copy_(base_weight);
            restored_layer->lora_a.copy_(restored.adapter_tensors.at("lora_a"));
            restored_layer->lora_b.copy_(restored.adapter_tensors.at("lora_b"));
        }
        layer->forward(second_input).pow(2).mean().backward();
        optimizer.step();
        restored_layer->forward(second_input).pow(2).mean().backward();
        restored_optimizer.step();
        require(torch::allclose(layer->lora_a, restored_layer->lora_a, 1e-6, 1e-6) &&
                    torch::allclose(layer->lora_b, restored_layer->lora_b, 1e-6, 1e-6),
                "resumed optimizer step differs from uninterrupted training");
        require(!torch::equal(checkpoint_a, layer->lora_a) || !torch::equal(checkpoint_b, layer->lora_b),
                "resume comparison did not perform another optimizer update");
        std::filesystem::rename(directory / "checkpoint", directory / "checkpoint.previous");
        imesvc::ml::LoraLinear recovered_layer(3, 4, config);
        torch::optim::AdamW recovered_optimizer(recovered_layer->adapter_parameters(), torch::optim::AdamWOptions(1e-2));
        const auto recovered = imesvc::ml::Checkpoint::load(directory / "checkpoint", recovered_optimizer);
        require(recovered.base_model_sha256 == "base-sha" && recovered.dataset_cursor == 9,
                "checkpoint previous-generation recovery failed");

        const auto adapter = directory / "adapter.gguf";
        imesvc::ml::GgufLoraWriter::write_f32_atomic(
            adapter,
            {{"blk.0.attn_q.weight.lora_a", torch::randn({2, 3})},
             {"blk.0.attn_q.weight.lora_b", torch::randn({4, 2})}},
            config);
        auto* gguf = gguf_init_from_file(adapter.c_str(), {.no_alloc = true, .ctx = nullptr});
        require(gguf != nullptr, "GGUF writer output could not be reopened");
        require(std::string(gguf_get_val_str(gguf, gguf_find_key(gguf, "general.type"))) == "adapter" &&
                    gguf_get_n_tensors(gguf) == 2 &&
                    std::abs(gguf_get_val_f32(gguf, gguf_find_key(gguf, "adapter.lora.alpha")) - 4.0F) < 1e-6F,
                "GGUF adapter metadata mismatch");
        gguf_free(gguf);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        throw;
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void test_sha256() {
    const auto directory = temporary_directory();
    const auto path = directory / "abc.txt";
    {
        std::ofstream output(path, std::ios::binary);
        output << "abc";
    }
    require(imesvc::ml::SafetensorsReader::sha256_file(path) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 known-answer test failed");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

}  // namespace

int main() {
    try {
        torch::manual_seed(20260713);
        test_rms_norm_and_rope();
        test_lora_gradients_and_math();
        test_gradient_accumulation_equivalence();
        test_attention_swiglu_and_causality();
        test_tokenizer_parity();
        test_safetensors_rejections();
        test_causal_loss_and_dataset();
        test_checkpoint_and_gguf();
        test_sha256();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ml_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
