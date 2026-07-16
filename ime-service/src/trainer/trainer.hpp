#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace imesvc::trainer {

struct TrainerOptions {
    std::filesystem::path base_safetensors, runtime_model, tables_directory, feedback_database, staging_directory,
        inference_heartbeat;
    std::string base_sha256, dataset_snapshot_id;
    std::string training_run_kind;
    std::uint32_t intraop_threads = 1, interop_threads = 1;
    std::uint64_t seed = 20260713;
    std::int64_t launch_heartbeat_millis = -1;
};

int run(const TrainerOptions& options);

}  // namespace imesvc::trainer
