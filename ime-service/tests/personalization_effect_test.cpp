#include "ml/training_tokenizer.hpp"
#include "trainer/trainer.hpp"
#include "training/feedback_store.hpp"

#include <llama.h>
#include <nlohmann/json.hpp>
#include <utf8/cpp20.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class BackendLifetime final {
public:
    BackendLifetime() { llama_backend_init(); }
    ~BackendLifetime() { llama_backend_free(); }
};

struct ModelDeleter {
    void operator()(llama_model* model) const noexcept { llama_model_free(model); }
};

struct ContextDeleter {
    void operator()(llama_context* context) const noexcept { llama_free(context); }
};

struct AdapterDeleter {
    void operator()(llama_adapter_lora* adapter) const noexcept {
        if (adapter != nullptr) llama_adapter_lora_free(adapter);
    }
};

struct CandidateScore {
    std::string character;
    double probability = 0.0;
};

struct Scenario {
    std::string context;
    std::string reading;
    std::string preferred_target;
    std::string target;
    std::string baseline_top1;
};

class RuntimeEvaluator final {
public:
    RuntimeEvaluator(const std::filesystem::path& model_path,
                     const std::filesystem::path& tables_directory)
        : tokenizer_(tables_directory) {
        std::ifstream candidate_input(tables_directory / "bopomofo_char.json");
        if (!candidate_input) throw std::runtime_error("open personalization candidate map failed");
        candidate_map_ = nlohmann::json::parse(candidate_input);
        auto parameters = llama_model_default_params();
        parameters.n_gpu_layers = 0;
        model_.reset(llama_model_load_from_file(model_path.c_str(), parameters));
        if (!model_) throw std::runtime_error("load deployed personalization fixture failed");
    }

    std::vector<CandidateScore> score(const Scenario& scenario,
                                      const std::optional<std::filesystem::path>& adapter_path = std::nullopt) const {
        const auto candidates = candidate_map_.at(scenario.reading).get<std::vector<std::string>>();
        std::vector<llama_token> candidate_tokens;
        std::vector<std::string> candidate_characters;
        for (const auto& candidate : candidates) {
            const auto scalars = utf8::utf8to32(candidate);
            if (scalars.size() != 1) continue;
            try {
                candidate_tokens.push_back(static_cast<llama_token>(tokenizer_.character_token(candidate)));
                candidate_characters.push_back(candidate);
            } catch (const std::exception&) {
            }
        }
        if (candidate_tokens.size() < 2) throw std::runtime_error("personalization scenario has fewer than two trainable candidates");

        std::vector<llama_token> prompt{static_cast<llama_token>(tokenizer_.special("<BOS>"))};
        const auto context_tokens = tokenizer_.encode_context(scenario.context);
        prompt.insert(prompt.end(), context_tokens.begin(), context_tokens.end());
        prompt.push_back(static_cast<llama_token>(tokenizer_.bopomofo_token(scenario.reading)));
        prompt.push_back(static_cast<llama_token>(tokenizer_.special("<SEP>")));

        std::unique_ptr<llama_adapter_lora, AdapterDeleter> adapter;
        if (adapter_path) {
            adapter.reset(llama_adapter_lora_init(model_.get(), adapter_path->c_str()));
            if (!adapter) throw std::runtime_error("load trained personalization adapter failed");
        }
        auto context_parameters = llama_context_default_params();
        context_parameters.n_ctx = 384;
        context_parameters.n_batch = static_cast<std::uint32_t>(prompt.size());
        context_parameters.n_ubatch = static_cast<std::uint32_t>(prompt.size());
        context_parameters.n_threads = 4;
        context_parameters.n_threads_batch = 4;
        context_parameters.no_perf = true;
        std::unique_ptr<llama_context, ContextDeleter> context(llama_init_from_model(model_.get(), context_parameters));
        if (!context) throw std::runtime_error("create personalization evaluation context failed");
        if (adapter) {
            auto* pointer = adapter.get();
            float scale = 1.0F;
            if (llama_set_adapters_lora(context.get(), &pointer, 1, &scale) != 0) {
                throw std::runtime_error("apply trained personalization adapter failed");
            }
        }

        auto batch = llama_batch_init(static_cast<std::int32_t>(prompt.size()), 0, 1);
        batch.n_tokens = static_cast<std::int32_t>(prompt.size());
        for (std::size_t index = 0; index < prompt.size(); ++index) {
            batch.token[index] = prompt[index];
            batch.pos[index] = static_cast<llama_pos>(index);
            batch.n_seq_id[index] = 1;
            batch.seq_id[index][0] = 0;
            batch.logits[index] = index + 1 == prompt.size() ? 1 : 0;
        }
        const int decoded = llama_decode(context.get(), batch);
        llama_batch_free(batch);
        if (decoded != 0) throw std::runtime_error("decode personalization evaluation prompt failed");
        const float* logits = llama_get_logits_ith(context.get(), -1);
        if (logits == nullptr) throw std::runtime_error("personalization evaluation logits are unavailable");

        double maximum = -std::numeric_limits<double>::infinity();
        for (const auto token : candidate_tokens) maximum = std::max(maximum, static_cast<double>(logits[token]));
        double denominator = 0.0;
        std::vector<CandidateScore> result;
        result.reserve(candidate_tokens.size());
        for (std::size_t index = 0; index < candidate_tokens.size(); ++index) {
            const double value = std::exp(static_cast<double>(logits[candidate_tokens[index]]) - maximum);
            denominator += value;
            result.push_back({candidate_characters[index], value});
        }
        if (!(denominator > 0.0) || !std::isfinite(denominator)) {
            throw std::runtime_error("personalization candidate normalization failed");
        }
        for (auto& candidate : result) candidate.probability /= denominator;
        std::stable_sort(result.begin(), result.end(), [](const CandidateScore& left, const CandidateScore& right) {
            return left.probability > right.probability;
        });
        return result;
    }

private:
    imesvc::ml::TrainingTokenizer tokenizer_;
    nlohmann::json candidate_map_;
    std::unique_ptr<llama_model, ModelDeleter> model_;
};

const CandidateScore& find_candidate(const std::vector<CandidateScore>& scores, const std::string& target) {
    const auto found = std::find_if(scores.begin(), scores.end(), [&target](const CandidateScore& candidate) {
        return candidate.character == target;
    });
    if (found == scores.end()) throw std::runtime_error("personalization target disappeared from legal candidates");
    return *found;
}

std::size_t candidate_rank(const std::vector<CandidateScore>& scores, const std::string& target) {
    const auto found = std::find_if(scores.begin(), scores.end(), [&target](const CandidateScore& candidate) {
        return candidate.character == target;
    });
    if (found == scores.end()) throw std::runtime_error("personalization target rank is unavailable");
    return static_cast<std::size_t>(std::distance(scores.begin(), found)) + 1U;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: personalization_effect_test MODEL_SAFETENSORS SHA256 RUNTIME_Q4_GGUF TABLES\n";
        return EXIT_FAILURE;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("llavon-ime-personalization-effect-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        BackendLifetime backend;
        RuntimeEvaluator evaluator(argv[3], argv[4]);
        const std::vector<Scenario> scenario_pool{
            {"我想", "ㄊㄚ ", "她", {}, {}},
            {"跑", "ㄉㄜ˙", "得", {}, {}},
            {"慢慢", "ㄉㄜ˙", "地", {}, {}},
            {"下次", "ㄗㄞˋ", "再", {}, {}},
            {"他現", "ㄗㄞˋ", "在", {}, {}},
            {"這件", "ㄕˋ", "事", {}, {}},
            {"這就", "ㄕˋ", "是", {}, {}},
            {"工", "ㄗㄨㄛˋ", "作", {}, {}},
            {"好", "ㄒㄧㄤˋ", "像", {}, {}},
            {"印", "ㄒㄧㄤˋ", "象", {}, {}},
            {"早", "ㄧˇ", "已", {}, {}},
            {"我叫王", "ㄨㄟˇ", "瑋", {}, {}},
            {"她叫陳", "ㄧˊ", "怡", {}, {}},
            {"朋友叫李", "ㄐㄩㄣˋ", "俊", {}, {}},
            {"同事叫張", "ㄊㄧㄥˊ", "婷", {}, {}},
            {"名字是林", "ㄑㄧˊ", "琪", {}, {}},
            {"孩子叫陳", "ㄩˇ", "宇", {}, {}},
            {"帳號是子", "ㄒㄩㄢ ", "軒", {}, {}},
            {"朋友叫志", "ㄏㄠˊ", "豪", {}, {}},
            {"同學叫文", "ㄐㄧㄝˊ", "傑", {}, {}},
        };
        std::vector<Scenario> scenarios;
        std::vector<std::vector<CandidateScore>> baseline_scores;
        for (auto scenario : scenario_pool) {
            auto scores = evaluator.score(scenario);
            scenario.baseline_top1 = scores.front().character;
            scenario.target = scenario.preferred_target;
            require(std::any_of(scores.begin(), scores.end(), [&scenario](const CandidateScore& candidate) {
                        return candidate.character == scenario.target;
                    }),
                    "intended common-correction target is not a legal candidate");
            if (scenario.target == scenario.baseline_top1) continue;
            scenarios.push_back(std::move(scenario));
            baseline_scores.push_back(std::move(scores));
            if (scenarios.size() == 5) break;
        }
        require(scenarios.size() == 5, "model fixture does not expose five intended common-correction mistakes");

        std::string snapshot_id;
        {
            imesvc::training::FeedbackStoreOptions store_options;
            store_options.data_directory = root / "data";
            store_options.queue_capacity = 1024;
            store_options.retention_check_interval = 0;
            imesvc::training::FeedbackStore store(std::move(store_options));
            require(store.set_learning_enabled(true).get().succeeded, "enable synthetic personalization store failed");
            constexpr std::size_t kExamplesPerScenario = 64;
            for (std::size_t scenario_index = 0; scenario_index < scenarios.size(); ++scenario_index) {
                const auto& scenario = scenarios[scenario_index];
                for (std::size_t example = 0; example < kExamplesPerScenario; ++example) {
                    imesvc::training::FeedbackEvent event;
                    event.event_id = "common-correction-" + std::to_string(scenario_index) + "-" + std::to_string(example);
                    event.left_context = scenario.context;
                    event.bopomofo_sequence = scenario.reading;
                    event.committed_characters = scenario.target;
                    event.predicted_top1 = scenario.baseline_top1;
                    event.manually_chosen_flags = {1};
                    event.signal_type = imesvc::training::FeedbackSignal::ExplicitCorrection;
                    event.base_model_hash = argv[2];
                    event.eligibility = imesvc::training::FeedbackEligibility::approved_sample();
                    require(store.enqueue(std::move(event)).accepted(), "enqueue synthetic common correction failed");
                }
            }
            require(store.flush().get().succeeded, "flush synthetic personalization feedback failed");
            const auto snapshot = store.create_dataset_snapshot().get();
            require(snapshot.operation.succeeded && snapshot.snapshot.training_target_characters > 0 &&
                        snapshot.snapshot.validation_target_characters > 0,
                    "create synthetic personalization snapshot failed");
            snapshot_id = snapshot.snapshot.snapshot_id;
            std::cout << "synthetic snapshot: samples=" << snapshot.snapshot.total_samples
                      << " train_chars=" << snapshot.snapshot.training_target_characters
                      << " validation_chars=" << snapshot.snapshot.validation_target_characters << '\n';
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
        std::filesystem::create_directories(root);
        {
            std::ofstream heartbeat(options.inference_heartbeat);
            heartbeat << "1\n";
        }
        options.launch_heartbeat_millis = 1;
        options.intraop_threads = 4;
        options.interop_threads = 1;
        require(imesvc::trainer::run(options) == 0, "synthetic personalization trainer did not complete");

        nlohmann::json manifest;
        {
            std::ifstream input(options.staging_directory / "manifest.json");
            if (!input) throw std::runtime_error("synthetic personalization manifest is missing");
            manifest = nlohmann::json::parse(input);
        }
        const double validation_before = manifest.at("validation_loss_before").get<double>();
        const double validation_after = manifest.at("validation_loss_after").get<double>();
        const auto adapter = options.staging_directory / "adapter.gguf";
        double negative_log_likelihood_before = 0.0;
        double negative_log_likelihood_after = 0.0;
        std::size_t top1_before = 0;
        std::size_t top1_after = 0;
        std::size_t probability_improvements = 0;
        std::size_t rank_improvements = 0;
        for (std::size_t index = 0; index < scenarios.size(); ++index) {
            const auto adapted = evaluator.score(scenarios[index], adapter);
            const auto before_probability = find_candidate(baseline_scores[index], scenarios[index].target).probability;
            const auto after_probability = find_candidate(adapted, scenarios[index].target).probability;
            const auto before_rank = candidate_rank(baseline_scores[index], scenarios[index].target);
            const auto after_rank = candidate_rank(adapted, scenarios[index].target);
            negative_log_likelihood_before -= std::log(before_probability);
            negative_log_likelihood_after -= std::log(after_probability);
            top1_before += before_rank == 1 ? 1U : 0U;
            top1_after += after_rank == 1 ? 1U : 0U;
            probability_improvements += after_probability > before_probability ? 1U : 0U;
            rank_improvements += after_rank < before_rank ? 1U : 0U;
            std::cout << "correction reading=" << scenarios[index].reading
                      << " predicted=" << scenarios[index].baseline_top1
                      << " target=" << scenarios[index].target
                      << " probability=" << before_probability << "->" << after_probability
                      << " rank=" << before_rank << "->" << after_rank << '\n';
        }
        negative_log_likelihood_before /= static_cast<double>(scenarios.size());
        negative_log_likelihood_after /= static_cast<double>(scenarios.size());
        std::cout << "validation_loss=" << validation_before << "->" << validation_after
                   << " constrained_nll=" << negative_log_likelihood_before << "->" << negative_log_likelihood_after
                   << " target_top1=" << top1_before << "->" << top1_after
                   << " probability_improvements=" << probability_improvements << '/' << scenarios.size()
                   << " rank_improvements=" << rank_improvements << '/' << scenarios.size() << '\n';

        require(std::isfinite(validation_before) && std::isfinite(validation_after) && validation_after < validation_before,
                "synthetic corrections did not improve held-out full-vocabulary validation loss");
        require(std::isfinite(negative_log_likelihood_before) && std::isfinite(negative_log_likelihood_after) &&
                    negative_log_likelihood_after < negative_log_likelihood_before,
                "synthetic corrections did not improve deployed constrained target likelihood");
        require(probability_improvements == scenarios.size(),
                "at least one synthetic common correction became less likely after personalization");
        require(top1_after >= top1_before, "synthetic corrections reduced target top-1 accuracy");
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "personalization_effect_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
