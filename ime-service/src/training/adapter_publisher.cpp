#include "training/adapter_publisher.hpp"

#include "engine/model_runtime.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace imesvc::training {
namespace {

StoreOperationResult success() { return {true, {}}; }
StoreOperationResult failure(std::string error) { return {false, std::move(error)}; }

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
        throw std::runtime_error("flush published LoRA artifact failed");
    }
    if (::close(descriptor) != 0) throw std::runtime_error("close published LoRA artifact failed");
#else
    (void)path;
#endif
}

constexpr std::uint64_t kMaximumAdapterBytes = 512ULL * 1024ULL * 1024ULL;

std::string sha256_bytes(std::vector<std::uint8_t> input) {
    constexpr std::array<std::uint32_t, 64> constants{0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
    const auto rotr = [](std::uint32_t value, unsigned int bits) { return (value >> bits) | (value << (32U - bits)); };
    const auto bit_count = static_cast<std::uint64_t>(input.size()) * 8U;
    input.push_back(0x80U);
    while (input.size() % 64U != 56U) input.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8) input.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    std::array<std::uint32_t, 8> state{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    for (std::size_t offset = 0; offset < input.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(input[offset + index * 4U]) << 24U) |
                           (static_cast<std::uint32_t>(input[offset + index * 4U + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(input[offset + index * 4U + 2U]) << 8U) |
                           input[offset + index * 4U + 3U];
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            words[index] = words[index - 16] + (rotr(words[index - 15], 7) ^ rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U)) +
                           words[index - 7] + (rotr(words[index - 2], 17) ^ rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U));
        }
        auto a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + constants[index] + words[index];
            const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto value : state) {
        for (int shift = 28; shift >= 0; shift -= 4) result.push_back(hex[(value >> shift) & 0xfU]);
    }
    return result;
}

std::string sha256_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumAdapterBytes) throw std::runtime_error("invalid LoRA adapter file size");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("open LoRA adapter failed");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("read LoRA adapter failed");
    return sha256_bytes(std::move(bytes));
}

bool safe_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && !std::filesystem::is_symlink(status) && std::filesystem::is_regular_file(status);
}

void create_private_directory(const std::filesystem::path& directory) {
    std::error_code error;
    const auto existing = std::filesystem::symlink_status(directory, error);
    if (!error && std::filesystem::is_symlink(existing)) throw std::runtime_error("adapter directory must not be a symlink");
    std::filesystem::create_directories(directory, error);
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        throw std::runtime_error("create adapter directory failed");
    }
#ifndef _WIN32
    if (::chmod(directory.c_str(), S_IRWXU) != 0) throw std::runtime_error("set adapter directory permissions failed");
#endif
}

void require_private_artifact(const std::filesystem::path& path) {
#ifndef _WIN32
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) throw std::runtime_error("set adapter artifact permissions failed");
#else
    (void)path;
#endif
}

std::string run_kind_name(TrainingRunKind kind) {
    switch (kind) {
        case TrainingRunKind::ShadowSmoke: return "shadow-smoke";
        case TrainingRunKind::FirstAdapter: return "first-adapter";
        case TrainingRunKind::Incremental: return "incremental";
    }
    throw std::runtime_error("invalid training run kind");
}

struct ValidatedStage {
    std::filesystem::path adapter;
    std::string adapter_sha256;
};

nlohmann::json read_manifest(const std::filesystem::path& path) {
    if (!safe_regular_file(path)) throw std::runtime_error("adapter manifest is not a regular file");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("open adapter manifest failed");
    return nlohmann::json::parse(input);
}

void validate_manifest_identity(const nlohmann::json& manifest, const AdapterPublisherOptions& options) {
    if (manifest.at("format_version").get<int>() != 2 ||
        manifest.at("base_model_sha256").get<std::string>() != options.base_model_sha256 ||
        manifest.at("base_model_revision").get<std::string>() != options.base_model_sha256 ||
        manifest.at("runtime_model_sha256").get<std::string>() != options.runtime_model_sha256 ||
        manifest.at("tokenizer_sha256").get<std::string>() != options.tokenizer_sha256 ||
        manifest.at("candidate_map_sha256").get<std::string>() != options.candidate_map_sha256 ||
        manifest.at("training_code_version").get<std::string>() != options.training_code_version ||
        manifest.at("seed").get<std::uint64_t>() != options.training_seed ||
        manifest.at("epochs").get<std::uint64_t>() != 1 || manifest.at("warmup_steps").get<std::uint64_t>() == 0 ||
        !std::isfinite(manifest.at("training_loss").get<double>()) || manifest.at("training_loss").get<double>() < 0.0 ||
        manifest.at("created_at").get<std::int64_t>() <= 0) {
        throw std::runtime_error("adapter manifest does not match the active model bundle");
    }
    const auto& tensors = manifest.at("tensor_metadata");
    if (!tensors.is_array() || tensors.size() != 20U * 7U * 2U) {
        throw std::runtime_error("adapter manifest tensor metadata is incomplete");
    }
    std::unordered_set<std::string> names;
    for (const auto& tensor : tensors) {
        const auto name = tensor.at("name").get<std::string>();
        const auto shape = tensor.at("shape").get<std::vector<std::int64_t>>();
        if (name.empty() || !names.insert(name).second || tensor.at("dtype").get<std::string>() != "F32" ||
            shape.size() != 2 || std::any_of(shape.begin(), shape.end(), [](std::int64_t dimension) { return dimension <= 0; })) {
            throw std::runtime_error("adapter manifest tensor metadata is invalid");
        }
    }
}

ValidatedStage validate_stage(const TrainingRunContext& run, const AdapterPublisherOptions& options, FeedbackStore& store) {
    if (run.run_id.empty() || run.snapshot_id.empty() || run.staging_directory.empty() || options.base_model_sha256.empty()) {
        throw std::runtime_error("incomplete training publication context");
    }
    const auto adapter = run.staging_directory / "adapter.gguf";
    const auto manifest_path = run.staging_directory / "manifest.json";
    if (!safe_regular_file(adapter) || !safe_regular_file(manifest_path)) throw std::runtime_error("trainer did not produce regular adapter artifacts");
    const auto manifest = read_manifest(manifest_path);
    validate_manifest_identity(manifest, options);
    const auto snapshot = store.load_dataset_snapshot(run.snapshot_id).get();
    if (!snapshot.operation.succeeded) throw std::runtime_error("load completed training snapshot failed: " + snapshot.operation.error);
    const auto before = manifest.at("validation_loss_before").get<double>();
    const auto after = manifest.at("validation_loss_after").get<double>();
    const auto validation_characters = manifest.at("validation_target_characters").get<std::uint64_t>();
    const auto validation_samples = manifest.at("validation_samples").get<std::uint64_t>();
    const auto rank = manifest.at("rank").get<std::uint64_t>();
    const auto alpha = manifest.at("alpha").get<double>();
    const auto steps = manifest.at("steps").get<std::uint64_t>();
    if (manifest.at("dataset_snapshot_sha256").get<std::string>() != snapshot.snapshot.sha256 ||
        manifest.at("training_run_kind").get<std::string>() != run_kind_name(run.kind) ||
        validation_characters != snapshot.snapshot.validation_target_characters ||
        validation_samples != snapshot.snapshot.validation_samples || validation_characters == 0 || rank == 0 ||
        !(alpha > 0.0) || steps == 0 || !std::isfinite(before) || !std::isfinite(after) || before < 0.0 || after < 0.0 ||
        (run.kind != TrainingRunKind::ShadowSmoke &&
         (validation_characters < options.minimum_validation_characters || !(after < before)))) {
        throw std::runtime_error("trained adapter did not pass the validation quality gate");
    }
    if (options.validate_loadable) options.validate_loadable(adapter);
    if (options.validate_runtime) {
        const auto runtime_validation = options.validate_runtime(adapter, snapshot.samples, run.kind);
        if (!runtime_validation.succeeded) throw std::runtime_error(runtime_validation.error);
    }
    return {adapter, sha256_file(adapter)};
}

}  // namespace

AdapterPublisher::AdapterPublisher(FeedbackStore& store, SharedModelRuntime& runtime, AdapterPublisherOptions options)
    : store_(store), runtime_(runtime), options_(std::move(options)) {
    if (options_.directory.empty()) options_.directory = store_.data_directory() / "adapters";
    options_.directory = std::filesystem::absolute(options_.directory);
    const auto data_directory = std::filesystem::absolute(store_.data_directory());
    const auto relative = options_.directory.lexically_relative(data_directory);
    if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..") {
        throw std::invalid_argument("published LoRA adapters must stay inside the private training data directory");
    }
    auto current = data_directory;
    for (const auto& component : relative) {
        current /= component;
        std::error_code path_error;
        const auto status = std::filesystem::symlink_status(current, path_error);
        if (!path_error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument("published LoRA adapter path contains a symlink component");
        }
    }
}

void AdapterPublisher::garbage_collect(std::string_view active_version) {
    std::error_code error;
    if (!std::filesystem::is_directory(options_.directory, error) || error) return;
    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(options_.directory, error)) {
        if (error) return;
        const auto name = entry.path().filename().string();
        if (name == active_version) continue;
        const auto status = entry.symlink_status(error);
        if (error) return;
        if (std::filesystem::is_symlink(status)) continue;
        if (!std::filesystem::is_directory(status)) continue;
        const auto modified = entry.last_write_time(error);
        if (error) return;
        candidates.push_back({entry.path(), modified});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.modified > right.modified;
    });
    const std::size_t inactive_to_keep = active_version.empty() ? 3U : 2U;
    for (std::size_t index = inactive_to_keep; index < candidates.size(); ++index) {
        std::filesystem::remove_all(candidates[index].path, error);
        if (error) return;
    }
}

StoreOperationResult AdapterPublisher::restore_active_adapter() {
    std::lock_guard lock(mutex_);
    std::string version;
    try {
        const auto record = store_.active_adapter().get();
        if (!record.operation.succeeded) return record.operation;
        if (!record.adapter) {
            garbage_collect({});
            return success();
        }
        version = record.adapter->version;
        const auto path = options_.directory / record.adapter->version / "adapter.gguf";
        const auto manifest_path = options_.directory / record.adapter->version / "manifest.json";
        if (record.adapter->base_model_hash != options_.base_model_sha256 || !safe_regular_file(path) ||
            !safe_regular_file(manifest_path) || sha256_file(path) != record.adapter->sha256) {
            (void)store_.deactivate_adapter(record.adapter->version).get();
            return failure("active LoRA adapter failed integrity validation and was disabled");
        }
        validate_manifest_identity(read_manifest(manifest_path), options_);
        if (options_.validate_loadable) options_.validate_loadable(path);
        runtime_.activate_adapter(record.adapter->version, path, record.adapter->sha256);
        garbage_collect(record.adapter->version);
        return success();
    } catch (const std::exception& error) {
        if (!version.empty()) (void)store_.deactivate_adapter(version).get();
        return failure(error.what());
    }
}

StoreOperationResult AdapterPublisher::handle_completed_run(const TrainingRunContext& run) {
    std::lock_guard lock(mutex_);
    try {
        const auto stage = validate_stage(run, options_, store_);
        if (run.kind == TrainingRunKind::ShadowSmoke) {
            std::error_code error;
            std::filesystem::remove_all(run.staging_directory, error);
            return error ? failure("remove successful shadow-smoke staging failed: " + error.message()) : success();
        }
        create_private_directory(options_.directory);
        const auto version = "lora-" + run.run_id + "-" + stage.adapter_sha256.substr(0, 16);
        const auto destination = options_.directory / version;
        if (std::filesystem::exists(destination)) return failure("refusing to overwrite an existing LoRA adapter version");
        std::error_code error;
        std::filesystem::rename(run.staging_directory, destination, error);
        if (error) return failure("atomic LoRA adapter publication failed: " + error.message());
        const auto published_adapter = destination / "adapter.gguf";
        const auto published_manifest = destination / "manifest.json";
        try {
            require_private_artifact(published_adapter);
            require_private_artifact(published_manifest);
            flush_path(published_adapter);
            flush_path(published_manifest);
            flush_path(destination);
            flush_path(options_.directory);
            const auto activate = [&]() -> StoreOperationResult {
                AdapterRecord record;
                record.version = version;
                record.base_model_hash = options_.base_model_sha256;
                record.dataset_snapshot_id = run.snapshot_id;
                record.sha256 = stage.adapter_sha256;
                record.active = true;
                const auto stored = store_.record_adapter_and_finish_training(std::move(record), run.run_id).get();
                if (!stored.succeeded) return stored;
                try {
                    runtime_.activate_adapter(version, published_adapter, stage.adapter_sha256);
                } catch (...) {
                    (void)store_.deactivate_adapter(version).get();
                    throw;
                }
                return success();
            };
            const auto activated = options_.transition_activation ? options_.transition_activation(run, activate) : activate();
            if (!activated.succeeded) {
                std::filesystem::remove_all(destination, error);
                return activated;
            }
            garbage_collect(version);
            return success();
        } catch (...) {
            std::filesystem::remove_all(destination, error);
            throw;
        }
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

StoreOperationResult AdapterPublisher::delete_all_artifacts() {
    std::lock_guard lock(mutex_);
    try {
        runtime_.clear_active_adapter();
        std::error_code error;
        std::filesystem::remove_all(options_.directory, error);
        return error ? failure("remove published LoRA adapters failed: " + error.message()) : success();
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

StoreOperationResult AdapterPublisher::evaluate_rollback() {
    std::lock_guard lock(mutex_);
    try {
        constexpr std::uint64_t kMinimumObservationCharacters = 5000;
        constexpr double kMaximumCorrectionRateRatio = 1.15;
        const auto active = store_.active_adapter().get();
        if (!active.operation.succeeded) return active.operation;
        if (!active.adapter) return success();
        const auto latest = store_.latest_adapter().get();
        if (!latest.operation.succeeded) return latest.operation;
        if (!latest.adapter || latest.adapter->version != active.adapter->version) return success();
        const auto current_stats = store_.adapter_feedback_stats(active.adapter->version).get();
        if (!current_stats.operation.succeeded) return current_stats.operation;
        if (current_stats.eligible_target_characters < kMinimumObservationCharacters) return success();
        const auto previous = store_.previous_adapter(active.adapter->version).get();
        if (!previous.operation.succeeded) return previous.operation;
        const auto previous_version = previous.adapter ? previous.adapter->version : std::string{};
        const auto previous_stats = store_.adapter_feedback_stats(previous_version).get();
        if (!previous_stats.operation.succeeded) return previous_stats.operation;
        if (previous_stats.eligible_target_characters < kMinimumObservationCharacters) return success();
        const auto current_rate = static_cast<double>(current_stats.correction_target_characters) /
                                  static_cast<double>(current_stats.eligible_target_characters);
        const auto previous_rate = static_cast<double>(previous_stats.correction_target_characters) /
                                   static_cast<double>(previous_stats.eligible_target_characters);
        if (current_rate <= previous_rate * kMaximumCorrectionRateRatio) return success();

        if (previous.adapter) {
            const auto path = options_.directory / previous.adapter->version / "adapter.gguf";
            const auto manifest = options_.directory / previous.adapter->version / "manifest.json";
            if (previous.adapter->base_model_hash != options_.base_model_sha256 || !safe_regular_file(path) ||
                !safe_regular_file(manifest) || sha256_file(path) != previous.adapter->sha256) {
                return failure("rollback adapter failed integrity validation");
            }
            validate_manifest_identity(read_manifest(manifest), options_);
            if (options_.validate_loadable) options_.validate_loadable(path);
            const auto stored = store_.activate_adapter(previous.adapter->version).get();
            if (!stored.succeeded) return stored;
            try {
                runtime_.activate_adapter(previous.adapter->version, path, previous.adapter->sha256);
            } catch (...) {
                (void)store_.activate_adapter(active.adapter->version).get();
                throw;
            }
        } else {
            const auto stored = store_.activate_adapter({}).get();
            if (!stored.succeeded) return stored;
            runtime_.clear_active_adapter();
        }
        return success();
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

}  // namespace imesvc::training
