#include "ml/checkpoint.hpp"
#include "training/sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

std::string read_checksum(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string checksum;
    std::string trailing;
    if (!(input >> checksum) || (input >> trailing) || checksum.size() != 64 ||
        !std::all_of(checksum.begin(), checksum.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        throw std::runtime_error("checkpoint metadata checksum is invalid");
    }
    return checksum;
}

nlohmann::json verified_metadata(const std::filesystem::path& source) {
    if (training::sha256_file(source / "state.json") != read_checksum(source / "state.sha256")) {
        throw std::runtime_error("checkpoint metadata integrity validation failed");
    }
    std::ifstream input(source / "state.json");
    if (!input) throw std::runtime_error("checkpoint metadata is missing");
    const auto metadata = nlohmann::json::parse(input);
    if (!metadata.contains("format_version") || metadata.at("format_version").get<int>() != 3 ||
        training::sha256_file(source / "adapter.pt") != metadata.at("adapter_sha256").get<std::string>() ||
        training::sha256_file(source / "optimizer.pt") != metadata.at("optimizer_sha256").get<std::string>()) {
        throw std::runtime_error("checkpoint artifact integrity validation failed");
    }
    return metadata;
}

bool valid_checkpoint(const std::filesystem::path& source) noexcept {
    try {
        (void)verified_metadata(source);
        return true;
    } catch (...) {
        return false;
    }
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
    const auto adapter_sha256 = training::sha256_file(temporary / "adapter.pt");
    const auto optimizer_sha256 = training::sha256_file(temporary / "optimizer.pt");
    nlohmann::json metadata{{"format_version", 3},
                             {"adapter_sha256", adapter_sha256}, {"optimizer_sha256", optimizer_sha256},
                             {"step", state.step}, {"scheduler_step", state.scheduler_step},
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
    {
        std::ofstream output(temporary / "state.json");
        if (!output) throw std::runtime_error("write checkpoint metadata failed");
        output << metadata;
        output.flush();
        if (!output) throw std::runtime_error("write checkpoint metadata failed");
    }
    {
        std::ofstream output(temporary / "state.sha256");
        if (!output) throw std::runtime_error("write checkpoint metadata checksum failed");
        output << training::sha256_file(temporary / "state.json") << '\n';
        output.flush();
        if (!output) throw std::runtime_error("write checkpoint metadata checksum failed");
    }
    flush_file(temporary / "adapter.pt");
    flush_file(temporary / "optimizer.pt");
    flush_file(temporary / "state.json");
    flush_file(temporary / "state.sha256");
    flush_file(temporary);

    const bool current_exists = std::filesystem::exists(directory, error) && !error;
    if (error) throw std::runtime_error("inspect current checkpoint failed: " + error.message());
    if (current_exists) {
        if (valid_checkpoint(directory)) {
            std::filesystem::remove_all(previous, error);
            if (error) throw std::runtime_error("remove stale previous checkpoint failed: " + error.message());
            std::filesystem::rename(directory, previous, error);
            if (error) throw std::runtime_error("preserve previous checkpoint failed: " + error.message());
        } else {
            std::filesystem::remove_all(directory, error);
            if (error) throw std::runtime_error("remove corrupt current checkpoint failed: " + error.message());
        }
    }
    error.clear();
    std::filesystem::rename(temporary, directory, error);
    if (error) {
        throw std::runtime_error("publish checkpoint failed: " + error.message());
    }
    flush_file(directory);
    if (!directory.parent_path().empty()) flush_file(directory.parent_path());
}

CheckpointState Checkpoint::load(const std::filesystem::path& directory, torch::optim::Optimizer& optimizer) {
    const auto load_source = [&optimizer](const std::filesystem::path& source) {
        const auto metadata = verified_metadata(source);
        CheckpointState state;
        state.step = metadata.at("step").get<std::uint64_t>();
        state.scheduler_step = metadata.at("scheduler_step").get<std::uint64_t>();
        state.epoch = metadata.at("epoch").get<std::uint64_t>();
        state.dataset_cursor = metadata.at("dataset_cursor").get<std::uint64_t>();
        state.dataset_snapshot_id = metadata.at("dataset_snapshot_id").get<std::string>();
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
        torch::serialize::InputArchive archive;
        archive.load_from(source / "adapter.pt");
        for (const auto& name : archive.keys()) {
            torch::Tensor tensor;
            archive.read(name, tensor);
            if (name == "__rng_state__") state.rng_state = std::move(tensor);
            else state.adapter_tensors.emplace(name, std::move(tensor));
        }
        torch::load(optimizer, source / "optimizer.pt");
        return state;
    };

    std::exception_ptr current_error;
    if (std::filesystem::is_regular_file(directory / "state.json")) {
        try {
            return load_source(directory);
        } catch (...) {
            current_error = std::current_exception();
        }
    }
    const std::filesystem::path previous = directory.string() + ".previous";
    if (std::filesystem::is_regular_file(previous / "state.json")) {
        auto state = load_source(previous);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        if (error) throw std::runtime_error("remove corrupt checkpoint before recovery failed: " + error.message());
        std::filesystem::rename(previous, directory, error);
        if (error) throw std::runtime_error("promote recovered checkpoint failed: " + error.message());
        if (!directory.parent_path().empty()) flush_file(directory.parent_path());
        return state;
    }
    if (current_error) std::rethrow_exception(current_error);
    throw std::runtime_error("checkpoint metadata is missing");
}

}  // namespace imesvc::ml
