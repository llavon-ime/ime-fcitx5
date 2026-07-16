#include "ml/checkpoint.hpp"
#include "ml/gguf_lora_writer.hpp"
#include "ml/lora_linear.hpp"
#include "ml/rms_norm.hpp"
#include "ml/rotary_embedding.hpp"
#include "ml/safetensors_reader.hpp"
#include "ml/training_dataset.hpp"

#include <gguf.h>
#include <ATen/CPUGeneratorImpl.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
        layer->forward(torch::randn({2, 3})).sum().backward();
        optimizer.step();

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
        test_causal_loss_and_dataset();
        test_checkpoint_and_gguf();
        test_sha256();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ml_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
