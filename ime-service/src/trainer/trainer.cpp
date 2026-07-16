#include "trainer/trainer.hpp"

#include "ml/checkpoint.hpp"
#include "ml/gguf_lora_writer.hpp"
#include "ml/llama_model.hpp"
#include "ml/safetensors_reader.hpp"
#include "ml/training_dataset.hpp"
#include "training/feedback_store.hpp"
#include "training/training_orchestrator.hpp"
#include "training/sha256.hpp"

#include <nlohmann/json.hpp>
#include <ATen/CPUGeneratorImpl.h>

#include <cmath>
#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <chrono>

namespace imesvc::trainer {
namespace {

class InferenceActivity final {};
constexpr std::string_view kTrainingCodeVersion = "native-libtorch-lora-v1";

double signal_weight(training::FeedbackSignal signal) {
    switch (signal) {
        case training::FeedbackSignal::ExplicitCorrection: return 3.0;
        case training::FeedbackSignal::ExplicitTop1Selection: return 1.5;
        case training::FeedbackSignal::AcceptedPrediction: return 1.0;
        case training::FeedbackSignal::FallbackCommit: return 0.5;
    }
    throw std::runtime_error("invalid feedback signal weight");
}

double validation_loss(ml::LlamaForCausalLm& model, const std::vector<training::DatasetSample>& samples,
                        const ml::TrainingTokenizer& tokenizer, const std::function<bool()>& interrupted) {
    torch::NoGradGuard guard;
    model->eval();
    double total = 0.0;
    std::size_t count = 0;
    for (const auto& sample : samples) {
        if (!sample.validation_member) continue;
        if (interrupted()) throw InferenceActivity{};
        const auto example = ml::make_training_example(sample, tokenizer);
        const auto loss = ml::causal_loss(model->forward(example.tokens), example.labels);
        if (!torch::isfinite(loss).item<bool>()) throw std::runtime_error("validation loss is not finite");
        total += loss.item<double>();
        ++count;
    }
    model->train();
    if (count == 0) throw std::runtime_error("training snapshot has no held-out validation samples");
    return total / static_cast<double>(count);
}

void restore_adapter_tensors(ml::LlamaForCausalLm& model,
                             const std::unordered_map<std::string, torch::Tensor>& saved) {
    const auto current = model->adapter_tensors();
    if (saved.size() != current.size()) throw std::runtime_error("checkpoint adapter tensor set mismatch");
    torch::NoGradGuard guard;
    for (const auto& [name, destination] : current) {
        const auto found = saved.find(name);
        if (found == saved.end() || found->second.sizes() != destination.sizes() ||
            found->second.scalar_type() != destination.scalar_type()) {
            throw std::runtime_error("checkpoint adapter tensor mismatch: " + name);
        }
        destination.copy_(found->second);
    }
}

void optimizer_step(ml::LlamaForCausalLm& model, torch::optim::Optimizer& optimizer, double total_weight) {
    if (!(total_weight > 0.0) || !std::isfinite(total_weight)) throw std::runtime_error("invalid gradient weight");
    const auto parameters = model->adapter_parameters();
    for (const auto& parameter : parameters) {
        if (parameter.grad().defined()) parameter.grad().div_(total_weight);
    }
    model->assert_gradient_isolation();
    (void)torch::nn::utils::clip_grad_norm_(parameters, 1.0, 2.0, true);
    optimizer.step();
    optimizer.zero_grad();
}

void set_learning_rate(torch::optim::Optimizer& optimizer, double learning_rate) {
    for (auto& group : optimizer.param_groups()) {
        static_cast<torch::optim::AdamWOptions&>(group.options()).lr(learning_rate);
    }
}

}  // namespace

int run(const TrainerOptions& options) {
    if (options.base_safetensors.empty() || options.runtime_model.empty() || options.base_sha256.empty() || options.tables_directory.empty() ||
        options.feedback_database.empty() || options.staging_directory.empty() || options.dataset_snapshot_id.empty() ||
        options.inference_heartbeat.empty() || options.launch_heartbeat_millis < 0) {
        throw std::invalid_argument("trainer requires base weights, SHA256, tables, snapshot database, snapshot ID, and staging directory");
    }
    torch::set_num_threads(static_cast<int>(options.intraop_threads));
    torch::set_num_interop_threads(static_cast<int>(options.interop_threads));
    torch::manual_seed(options.seed);
    at::globalContext().setDeterministicAlgorithms(true, false);
    const auto interrupted = [&options]() {
        return training::TrainingOrchestrator::inference_activity_since(options.inference_heartbeat,
                                                                         options.launch_heartbeat_millis);
    };
    if (interrupted()) return 2;
    const auto runtime_model_sha256 = training::sha256_file(options.runtime_model);
    const auto tokenizer_sha256 = training::sha256_file_set(
        options.tables_directory,
        {"tokens/chars.json", "tokens/latin.json", "tokens/special_tokens.json", "tokens/bpmf.json"});
    const auto candidate_map_sha256 = training::sha256_file(options.tables_directory / "bopomofo_char.json");
    if (interrupted()) return 2;
    training::FeedbackStoreOptions store_options;
    store_options.data_directory = options.feedback_database.parent_path();
    store_options.database_filename = options.feedback_database.filename().string();
    training::FeedbackStore store(std::move(store_options));
    const auto snapshot = store.load_dataset_snapshot(options.dataset_snapshot_id).get();
    if (!snapshot.operation.succeeded) throw std::runtime_error("load immutable dataset snapshot failed: " + snapshot.operation.error);
    if (std::any_of(snapshot.samples.begin(), snapshot.samples.end(), [&options](const auto& sample) {
            return sample.base_model_hash != options.base_sha256;
        })) {
        throw std::runtime_error("dataset snapshot contains feedback for a different base model");
    }
    ml::LlamaForCausalLm model;
    ml::SafetensorsReader().load_fixed_llama(options.base_safetensors, options.base_sha256, model->safetensors_parameters());
    if (interrupted()) return 2;
    model->freeze_base_parameters();
    torch::optim::AdamW optimizer(model->adapter_parameters(), torch::optim::AdamWOptions(5e-5).weight_decay(0.01));
    const auto checkpoint_directory = options.staging_directory / "checkpoint";
    std::uint64_t step = 0;
    std::size_t dataset_cursor = 0;
    double accumulated_training_loss = 0.0;
    double accumulated_training_weight = 0.0;
    std::optional<double> original_validation_loss;
    const std::filesystem::path previous_checkpoint_directory = checkpoint_directory.string() + ".previous";
    if (std::filesystem::is_directory(checkpoint_directory) || std::filesystem::is_directory(previous_checkpoint_directory)) {
        const auto checkpoint = ml::Checkpoint::load(checkpoint_directory, optimizer);
        if (checkpoint.dataset_snapshot_id != options.dataset_snapshot_id || checkpoint.base_model_sha256 != options.base_sha256 ||
            checkpoint.runtime_model_sha256 != runtime_model_sha256 || checkpoint.tokenizer_sha256 != tokenizer_sha256 ||
            checkpoint.candidate_map_sha256 != candidate_map_sha256 || checkpoint.training_code_version != kTrainingCodeVersion ||
            checkpoint.lora_rank != 8 || checkpoint.lora_alpha != 16.0 || checkpoint.lora_dropout != 0.05 ||
            checkpoint.dataset_cursor > snapshot.samples.size() || checkpoint.scheduler_step != checkpoint.step ||
            checkpoint.seed != options.seed || checkpoint.epoch > 1 ||
            !std::isfinite(checkpoint.accumulated_training_loss) || checkpoint.accumulated_training_loss < 0.0 ||
            !std::isfinite(checkpoint.accumulated_training_weight) || checkpoint.accumulated_training_weight < 0.0) {
            throw std::runtime_error("checkpoint does not match the immutable dataset snapshot");
        }
        restore_adapter_tensors(model, checkpoint.adapter_tensors);
        if (checkpoint.rng_state.defined()) {
            auto generator = at::detail::getDefaultCPUGenerator();
            generator.set_state(checkpoint.rng_state);
        }
        step = checkpoint.step;
        dataset_cursor = static_cast<std::size_t>(checkpoint.dataset_cursor);
        original_validation_loss = checkpoint.validation_loss_before;
        accumulated_training_loss = checkpoint.accumulated_training_loss;
        accumulated_training_weight = checkpoint.accumulated_training_weight;
    }
    ml::TrainingTokenizer tokenizer(options.tables_directory);
    double validation_loss_before = 0.0;
    try {
        validation_loss_before = original_validation_loss ? *original_validation_loss
                                                          : validation_loss(model, snapshot.samples, tokenizer, interrupted);
    } catch (const InferenceActivity&) {
        return 2;
    }
    if (!std::isfinite(validation_loss_before) || validation_loss_before < 0.0) {
        throw std::runtime_error("checkpoint validation baseline is invalid");
    }
    model->train();
    const auto training_samples = static_cast<std::uint64_t>(std::count_if(
        snapshot.samples.begin(), snapshot.samples.end(), [](const auto& sample) { return !sample.validation_member; }));
    const auto total_steps = (training_samples + 63U) / 64U;
    const auto warmup_steps = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(total_steps * 0.03)));
    if (step > total_steps) throw std::runtime_error("checkpoint training step exceeds the immutable dataset");
    const auto save_checkpoint = [&](std::size_t cursor, std::uint64_t epoch) {
        ml::CheckpointState checkpoint;
        checkpoint.adapter_tensors = model->adapter_tensors();
        checkpoint.rng_state = at::detail::getDefaultCPUGenerator().get_state();
        checkpoint.step = step;
        checkpoint.scheduler_step = step;
        checkpoint.epoch = epoch;
        checkpoint.dataset_cursor = cursor;
        checkpoint.seed = options.seed;
        checkpoint.dataset_snapshot_id = options.dataset_snapshot_id;
        checkpoint.base_model_sha256 = options.base_sha256;
        checkpoint.runtime_model_sha256 = runtime_model_sha256;
        checkpoint.tokenizer_sha256 = tokenizer_sha256;
        checkpoint.candidate_map_sha256 = candidate_map_sha256;
        checkpoint.training_code_version = kTrainingCodeVersion;
        checkpoint.lora_rank = 8;
        checkpoint.lora_alpha = 16.0;
        checkpoint.lora_dropout = 0.05;
        checkpoint.validation_loss_before = validation_loss_before;
        checkpoint.accumulated_training_loss = accumulated_training_loss;
        checkpoint.accumulated_training_weight = accumulated_training_weight;
        ml::Checkpoint::save_atomic(checkpoint_directory, checkpoint, optimizer);
    };
    std::uint64_t accumulated = 0;
    double accumulated_weight = 0.0;
    std::size_t accumulation_start = dataset_cursor;
    double loss_before_accumulation = accumulated_training_loss;
    double weight_before_accumulation = accumulated_training_weight;
    torch::Tensor rng_before_accumulation;
    const bool had_training_work = std::any_of(snapshot.samples.begin() + static_cast<std::ptrdiff_t>(dataset_cursor), snapshot.samples.end(),
                                               [](const auto& sample) { return !sample.validation_member; });
    for (std::size_t index = dataset_cursor; index < snapshot.samples.size(); ++index) {
        const auto& sample = snapshot.samples[index];
        if (sample.validation_member) continue;
        if (accumulated == 0) {
            accumulation_start = index;
            loss_before_accumulation = accumulated_training_loss;
            weight_before_accumulation = accumulated_training_weight;
            rng_before_accumulation = at::detail::getDefaultCPUGenerator().get_state();
        }
        if (interrupted()) {
            optimizer.zero_grad();
            accumulated_training_loss = loss_before_accumulation;
            accumulated_training_weight = weight_before_accumulation;
            if (rng_before_accumulation.defined()) {
                auto generator = at::detail::getDefaultCPUGenerator();
                generator.set_state(rng_before_accumulation);
            }
            save_checkpoint(accumulation_start, 0);
            return 2;
        }
        const auto example = ml::make_training_example(sample, tokenizer);
        const auto weight = signal_weight(sample.signal_type);
        const auto loss = ml::causal_loss(model->forward(example.tokens), example.labels);
        if (!torch::isfinite(loss).item<bool>()) throw std::runtime_error("training loss is not finite");
        (loss * weight).backward();
        accumulated_training_loss += loss.item<double>() * weight;
        accumulated_training_weight += weight;
        accumulated_weight += weight;
        if (++accumulated != 64) continue;
        const auto next_step = step + 1;
        set_learning_rate(optimizer, 5e-5 * std::min(1.0, static_cast<double>(next_step) / static_cast<double>(warmup_steps)));
        optimizer_step(model, optimizer, accumulated_weight);
        ++step;
        accumulated = 0;
        accumulated_weight = 0.0;
        if (interrupted()) {
            save_checkpoint(index + 1, 0);
            return 2;
        }
    }
    if (accumulated != 0) {
        const auto next_step = step + 1;
        set_learning_rate(optimizer, 5e-5 * std::min(1.0, static_cast<double>(next_step) / static_cast<double>(warmup_steps)));
        optimizer_step(model, optimizer, accumulated_weight);
        ++step;
    }
    if (had_training_work &&
        interrupted()) {
        save_checkpoint(snapshot.samples.size(), 1);
        return 2;
    }
    double validation_loss_after = 0.0;
    try {
        validation_loss_after = validation_loss(model, snapshot.samples, tokenizer, interrupted);
    } catch (const InferenceActivity&) {
        save_checkpoint(snapshot.samples.size(), 1);
        return 2;
    }
    std::error_code error; std::filesystem::create_directories(options.staging_directory, error);
    if (error) throw std::runtime_error("create adapter staging directory failed: " + error.message());
    const auto adapter_tensors = model->gguf_adapter_tensors();
    ml::GgufLoraWriter::write_f32_atomic(options.staging_directory / "adapter.gguf", adapter_tensors, {});
    if (interrupted()) {
        std::filesystem::remove(options.staging_directory / "adapter.gguf", error);
        save_checkpoint(snapshot.samples.size(), 1);
        return 2;
    }
    nlohmann::json tensor_metadata = nlohmann::json::array();
    for (const auto& [name, tensor] : adapter_tensors) {
        std::vector<std::int64_t> shape(tensor.sizes().begin(), tensor.sizes().end());
        tensor_metadata.push_back({{"name", name}, {"shape", shape}, {"dtype", "F32"}});
    }
    nlohmann::json manifest{{"format_version", 2},
                            {"base_model_sha256", options.base_sha256},
                            {"base_model_revision", options.base_sha256},
                            {"runtime_model_sha256", runtime_model_sha256},
                            {"tokenizer_sha256", tokenizer_sha256},
                            {"candidate_map_sha256", candidate_map_sha256},
                            {"dataset_snapshot_sha256", snapshot.snapshot.sha256},
                            {"training_code_version", kTrainingCodeVersion},
                            {"training_run_kind", options.training_run_kind},
                            {"seed", options.seed},
                            {"rank", 8},
                            {"alpha", 16},
                            {"tensor_metadata", std::move(tensor_metadata)},
                            {"created_at", std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch()).count()},
                            {"steps", step},
                            {"epochs", 1},
                            {"warmup_steps", warmup_steps},
                            {"training_loss", accumulated_training_weight > 0.0
                                                  ? accumulated_training_loss / accumulated_training_weight
                                                  : 0.0},
                            {"validation_samples", snapshot.snapshot.validation_samples},
                            {"validation_target_characters", snapshot.snapshot.validation_target_characters},
                            {"validation_loss_before", validation_loss_before},
                            {"validation_loss_after", validation_loss_after}};
    std::ofstream output(options.staging_directory / "manifest.json");
    if (!output) throw std::runtime_error("write adapter manifest failed");
    output << manifest;
    output.close();
    if (!output) throw std::runtime_error("flush adapter manifest failed");
    std::filesystem::remove_all(checkpoint_directory, error);
    if (error) throw std::runtime_error("remove completed checkpoint failed: " + error.message());
    std::filesystem::remove_all(previous_checkpoint_directory, error);
    if (error) throw std::runtime_error("remove recovered checkpoint failed: " + error.message());
    return 0;
}

}  // namespace imesvc::trainer
