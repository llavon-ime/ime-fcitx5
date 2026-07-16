#include "ml/checkpoint.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace imesvc::ml {
namespace {
void flush_file(const std::filesystem::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0 || ::fsync(fd) != 0) { if (fd >= 0) ::close(fd); throw std::runtime_error("flush checkpoint file failed"); }
    ::close(fd);
#else
    (void)path;
#endif
}
}  // namespace

void Checkpoint::save_atomic(const std::filesystem::path& directory, const CheckpointState& state,
                             torch::optim::Optimizer& optimizer) {
    const std::filesystem::path temporary = directory.string() + ".tmp";
    const std::filesystem::path previous = directory.string() + ".previous";
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary, error);
    if (error) throw std::runtime_error("create checkpoint staging directory failed");
    torch::serialize::OutputArchive archive;
    for (const auto& [name, tensor] : state.adapter_tensors) archive.write(name, tensor.detach().cpu(), false);
    if (state.rng_state.defined()) archive.write("__rng_state__", state.rng_state.detach().cpu(), false);
    archive.save_to(temporary / "adapter.pt");
    torch::save(optimizer, temporary / "optimizer.pt");
    nlohmann::json metadata{{"step", state.step}, {"scheduler_step", state.scheduler_step},
                             {"epoch", state.epoch}, {"dataset_cursor", state.dataset_cursor}, {"seed", state.seed},
                             {"validation_loss_before", state.validation_loss_before},
                             {"accumulated_training_loss", state.accumulated_training_loss},
                             {"accumulated_training_weight", state.accumulated_training_weight},
                             {"dataset_snapshot_id", state.dataset_snapshot_id},
                             {"base_model_sha256", state.base_model_sha256},
                             {"runtime_model_sha256", state.runtime_model_sha256},
                             {"tokenizer_sha256", state.tokenizer_sha256},
                             {"candidate_map_sha256", state.candidate_map_sha256},
                             {"training_code_version", state.training_code_version},
                             {"lora_rank", state.lora_rank}, {"lora_alpha", state.lora_alpha},
                             {"lora_dropout", state.lora_dropout}};
    { std::ofstream output(temporary / "state.json"); if (!output) throw std::runtime_error("write checkpoint metadata failed"); output << metadata; }
    flush_file(temporary / "adapter.pt"); flush_file(temporary / "optimizer.pt"); flush_file(temporary / "state.json");
    std::filesystem::remove_all(previous, error); error.clear();
    if (std::filesystem::exists(directory, error) && !error) {
        std::filesystem::rename(directory, previous, error);
        if (error) throw std::runtime_error("preserve previous checkpoint failed: " + error.message());
    }
    error.clear();
    std::filesystem::rename(temporary, directory, error);
    if (error) {
        std::error_code restore_error;
        if (std::filesystem::exists(previous, restore_error) && !restore_error) {
            std::filesystem::rename(previous, directory, restore_error);
        }
        throw std::runtime_error("publish checkpoint failed: " + error.message());
    }
    flush_file(directory);
    if (!directory.parent_path().empty()) flush_file(directory.parent_path());
    std::filesystem::remove_all(previous, error);
}

CheckpointState Checkpoint::load(const std::filesystem::path& directory, torch::optim::Optimizer& optimizer) {
    auto source = directory;
    if (!std::filesystem::is_regular_file(source / "state.json")) {
        const std::filesystem::path previous = directory.string() + ".previous";
        if (std::filesystem::is_regular_file(previous / "state.json")) source = previous;
    }
    std::ifstream input(source / "state.json");
    if (!input) throw std::runtime_error("checkpoint metadata is missing");
    const auto metadata = nlohmann::json::parse(input);
    CheckpointState state;
    state.step = metadata.at("step").get<std::uint64_t>();
    state.scheduler_step = metadata.at("scheduler_step").get<std::uint64_t>();
    state.epoch = metadata.at("epoch").get<std::uint64_t>();
    state.dataset_cursor = metadata.at("dataset_cursor").get<std::uint64_t>(); state.dataset_snapshot_id = metadata.at("dataset_snapshot_id").get<std::string>();
    state.base_model_sha256 = metadata.at("base_model_sha256").get<std::string>();
    state.runtime_model_sha256 = metadata.at("runtime_model_sha256").get<std::string>();
    state.tokenizer_sha256 = metadata.at("tokenizer_sha256").get<std::string>();
    state.candidate_map_sha256 = metadata.at("candidate_map_sha256").get<std::string>();
    state.training_code_version = metadata.at("training_code_version").get<std::string>();
    state.lora_rank = metadata.at("lora_rank").get<std::int64_t>();
    state.lora_alpha = metadata.at("lora_alpha").get<double>();
    state.lora_dropout = metadata.at("lora_dropout").get<double>();
    state.seed = metadata.at("seed").get<std::uint64_t>();
    state.validation_loss_before = metadata.at("validation_loss_before").get<double>();
    state.accumulated_training_loss = metadata.at("accumulated_training_loss").get<double>();
    state.accumulated_training_weight = metadata.at("accumulated_training_weight").get<double>();
    torch::serialize::InputArchive archive; archive.load_from(source / "adapter.pt");
    for (const auto& name : archive.keys()) {
        torch::Tensor tensor;
        archive.read(name, tensor);
        if (name == "__rng_state__") state.rng_state = std::move(tensor);
        else state.adapter_tensors.emplace(name, std::move(tensor));
    }
    torch::load(optimizer, source / "optimizer.pt");
    return state;
}

}  // namespace imesvc::ml
