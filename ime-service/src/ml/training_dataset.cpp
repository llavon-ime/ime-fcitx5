#include "ml/training_dataset.hpp"

#include <torch/nn/functional/loss.h>

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace imesvc::ml {
namespace {

std::vector<std::string> utf8_scalars(const std::string& value) {
    std::vector<std::string> result;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto first = static_cast<unsigned char>(value[offset]);
        const std::size_t width = first <= 0x7fU ? 1U : first <= 0xdfU ? 2U : first <= 0xefU ? 3U : 4U;
        if ((width == 4U && first > 0xf4U) || offset + width > value.size()) throw std::runtime_error("invalid UTF-8 training sample");
        for (std::size_t i = 1; i < width; ++i) if ((static_cast<unsigned char>(value[offset + i]) & 0xc0U) != 0x80U) throw std::runtime_error("invalid UTF-8 training sample");
        result.emplace_back(value.substr(offset, width));
        offset += width;
    }
    return result;
}

}  // namespace

std::vector<std::int64_t> TrainingTokenizer::encode_context(std::string_view text) const {
    std::vector<std::int64_t> result;
    const auto scalars = utf8_scalars(std::string(text));
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        if (scalars[index] == " ") { result.push_back(special("<SP>")); continue; }
        if (const auto known = chars_.find(scalars[index]); known != chars_.end()) { result.push_back(known->second); continue; }
        const auto latin = scalars[index].size() == 1 && (std::isalnum(static_cast<unsigned char>(scalars[index][0])) || scalars[index][0] == '-' || scalars[index][0] == '_' || scalars[index][0] == '+');
        if (!latin) { result.push_back(special("<UNK>")); continue; }
        std::string run;
        do { run += scalars[index]; ++index; } while (index < scalars.size() && scalars[index].size() == 1 && (std::isalnum(static_cast<unsigned char>(scalars[index][0])) || scalars[index][0] == '-' || scalars[index][0] == '_' || scalars[index][0] == '+'));
        --index;
        for (auto& byte : run) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
        result.push_back(latin_.contains(run) ? latin_.at(run) : special("<LATIN>"));
    }
    while (!result.empty() && result.front() == special("<UNK>")) result.erase(result.begin());
    return result;
}

TrainingExample make_training_example(const training::DatasetSample& sample, const TrainingTokenizer& tokenizer,
                                      const LlamaModelConfig& config) {
    config.validate();
    const auto targets = utf8_scalars(sample.committed_characters);
    std::istringstream readings(sample.bopomofo_sequence);
    std::vector<std::string> reading_tokens;
    std::string reading;
    while (std::getline(readings, reading, '\x1f')) {
        if (reading.empty()) throw std::runtime_error("training sample contains an empty Bopomofo reading");
        reading_tokens.push_back(std::move(reading));
    }
    if (targets.empty() || targets.size() != reading_tokens.size()) throw std::runtime_error("training sample is not one-to-one aligned");
    auto context = tokenizer.encode_context(sample.left_context);
    std::vector<std::int64_t> suffix{tokenizer.special("<BOS>")};
    suffix.insert(suffix.end(), context.begin(), context.end());
    for (const auto& reading : reading_tokens) suffix.push_back(tokenizer.bopomofo_token(reading));
    suffix.push_back(tokenizer.special("<SEP>"));
    for (const auto& target : targets) suffix.push_back(tokenizer.character_token(target));
    const auto protected_tokens = suffix.size() - context.size();
    if (protected_tokens > static_cast<std::size_t>(config.context_length)) throw std::runtime_error("reading and target exceed model context window");
    if (suffix.size() > static_cast<std::size_t>(config.context_length)) suffix.erase(suffix.begin() + 1, suffix.begin() + 1 + static_cast<std::ptrdiff_t>(suffix.size() - config.context_length));
    std::vector<std::int64_t> labels(suffix.size(), kIgnoreLabel);
    const auto first_target = suffix.size() - targets.size();
    for (std::size_t index = first_target; index < suffix.size(); ++index) labels[index] = suffix[index];
    return {torch::tensor(suffix, torch::kInt64).unsqueeze(0), torch::tensor(labels, torch::kInt64).unsqueeze(0)};
}

torch::Tensor causal_loss(const torch::Tensor& logits, const torch::Tensor& labels) {
    namespace F = torch::nn::functional;
    if (logits.dim() != 3 || labels.dim() != 2 || labels.size(0) != logits.size(0) || labels.size(1) != logits.size(1))
        throw std::invalid_argument("invalid causal loss shapes");
    return F::cross_entropy(logits.slice(1, 0, -1).reshape({-1, logits.size(-1)}), labels.slice(1, 1).reshape({-1}),
                            F::CrossEntropyFuncOptions().ignore_index(kIgnoreLabel));
}

}  // namespace imesvc::ml
