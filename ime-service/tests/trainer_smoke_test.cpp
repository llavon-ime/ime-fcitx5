#include "ml/gguf_lora_writer.hpp"
#include "trainer/trainer.hpp"
#include "training/feedback_store.hpp"

#include <llama.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

struct ModelDeleter {
    void operator()(llama_model* model) const { llama_model_free(model); }
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: trainer_smoke_test MODEL_SAFETENSORS SHA256 F16_GGUF TABLES\n";
        return EXIT_FAILURE;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("llavon-ime-trainer-smoke-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::string snapshot_id;
        {
            imesvc::training::FeedbackStoreOptions store_options;
            store_options.data_directory = root / "data";
            imesvc::training::FeedbackStore store(store_options);
            require(store.set_learning_enabled(true).get().succeeded, "could not enable smoke-test feedback store");
            std::string event_id = "trainer-smoke-0";
            for (int index = 1; imesvc::training::FeedbackStore::deterministic_validation_member(event_id); ++index) {
                event_id = "trainer-smoke-" + std::to_string(index);
            }
            imesvc::training::FeedbackEvent event;
            event.event_id = event_id;
            event.left_context = "你好";
            event.bopomofo_sequence = "ㄋㄧˇ";
            event.committed_characters = "你";
            event.predicted_top1 = "你";
            event.manually_chosen_flags = {0};
            event.signal_type = imesvc::training::FeedbackSignal::ExplicitCorrection;
            event.base_model_hash = argv[2];
            event.created_at_unix_seconds = 1;
            event.eligibility = imesvc::training::FeedbackEligibility::approved_sample();
            require(store.enqueue(event).accepted(), "could not enqueue smoke-test feedback");
            std::string validation_id = "trainer-smoke-validation-0";
            for (int index = 1; !imesvc::training::FeedbackStore::deterministic_validation_member(validation_id); ++index) {
                validation_id = "trainer-smoke-validation-" + std::to_string(index);
            }
            event.event_id = validation_id;
            event.created_at_unix_seconds = 2;
            require(store.enqueue(std::move(event)).accepted(), "could not enqueue smoke-test validation feedback");
            require(store.flush().get().succeeded, "could not flush smoke-test feedback");
            const auto snapshot = store.create_dataset_snapshot().get();
            require(snapshot.operation.succeeded && snapshot.snapshot.training_target_characters == 1 &&
                        snapshot.snapshot.validation_target_characters == 1,
                    "could not create smoke-test training snapshot");
            snapshot_id = snapshot.snapshot.snapshot_id;
        }

        imesvc::trainer::TrainerOptions options;
        options.base_safetensors = argv[1];
        options.runtime_model = argv[3];
        options.base_sha256 = argv[2];
        options.tables_directory = argv[4];
        options.feedback_database = root / "data" / "feedback.sqlite3";
        options.dataset_snapshot_id = snapshot_id;
        options.staging_directory = root / "staging";
        options.inference_heartbeat = root / "inference.heartbeat";
        {
            std::ofstream heartbeat(options.inference_heartbeat);
            heartbeat << "1\n";
        }
        options.launch_heartbeat_millis = 1;
        options.intraop_threads = 4;
        options.interop_threads = 1;
        require(imesvc::trainer::run(options) == 0, "trainer did not complete the smoke run");
        require(std::filesystem::is_regular_file(options.staging_directory / "adapter.gguf") &&
                    std::filesystem::is_regular_file(options.staging_directory / "manifest.json"),
                "trainer did not emit staged adapter artifacts");

        llama_backend_init();
        auto model_parameters = llama_model_default_params();
        model_parameters.n_gpu_layers = 0;
        std::unique_ptr<llama_model, ModelDeleter> model(llama_model_load_from_file(argv[3], model_parameters));
        require(model != nullptr, "could not load llama.cpp smoke fixture");
        imesvc::ml::GgufLoraWriter::validate_loadable(options.staging_directory / "adapter.gguf", model.get());
        llama_backend_free();
        std::filesystem::remove_all(root);
        std::cout << "trainer smoke test completed with snapshot " << snapshot_id << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "trainer_smoke_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
