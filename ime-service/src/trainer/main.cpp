#include "trainer/trainer.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
imesvc::trainer::TrainerOptions parse(int argc, char** argv) {
    imesvc::trainer::TrainerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto value = [&]() -> std::string_view { if (++index >= argc) throw std::runtime_error("missing trainer option value"); return argv[index]; };
        if (argument == "--base-safetensors") options.base_safetensors = value();
        else if (argument == "--runtime-model") options.runtime_model = value();
        else if (argument == "--base-sha256") options.base_sha256 = value();
        else if (argument == "--tables") options.tables_directory = value();
        else if (argument == "--feedback-database") options.feedback_database = value();
        else if (argument == "--dataset-snapshot-id") options.dataset_snapshot_id = value();
        else if (argument == "--staging-directory") options.staging_directory = value();
        else if (argument == "--inference-heartbeat") options.inference_heartbeat = value();
        else if (argument == "--training-run-kind") options.training_run_kind = value();
        else if (argument == "--intraop-threads") options.intraop_threads = static_cast<std::uint32_t>(std::stoul(std::string(value())));
        else if (argument == "--interop-threads") options.interop_threads = static_cast<std::uint32_t>(std::stoul(std::string(value())));
        else if (argument == "--seed") options.seed = std::stoull(std::string(value()));
        else if (argument == "--launch-heartbeat-millis") options.launch_heartbeat_millis = std::stoll(std::string(value()));
        else throw std::runtime_error("unknown trainer option: " + std::string(argument));
    }
    return options;
}
}

int main(int argc, char** argv) {
    try { return imesvc::trainer::run(parse(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "llavon-ime-trainer: " << error.what() << '\n'; return 1; }
}
