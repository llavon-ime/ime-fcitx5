#include "service/llama_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bopomofo/table_engine.hpp"
#include "service/token_filter.hpp"
#include "text/utf.hpp"

#ifndef IME_FCITX5_HAS_LLAMA
#define IME_FCITX5_HAS_LLAMA 0
#endif

#if IME_FCITX5_HAS_LLAMA
#include <llama.h>
#endif

namespace ime::fcitx5 {

namespace {

std::filesystem::path project_root_from_source() {
    std::filesystem::path source_path(__FILE__);
    if (source_path.is_relative()) source_path = std::filesystem::absolute(source_path);
    return source_path.parent_path().parent_path().parent_path();
}

std::filesystem::path tables_dir() {
    if (const char* override = std::getenv("IME_FCITX5_TABLE_DIR"); override != nullptr && override[0] != '\0')
        return override;

#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_path = std::filesystem::path(home) / "Library" / "fcitx5" / "share" / "llavon-ime" / "tables";
        if (std::filesystem::exists(user_path / "tokens")) return user_path;
    }
#endif

#ifdef IME_FCITX5_INSTALLED_TABLE_DIR
    const auto installed_path = std::filesystem::path(IME_FCITX5_INSTALLED_TABLE_DIR);
    if (std::filesystem::exists(installed_path / "tokens")) return installed_path;
#endif

#ifdef IME_FCITX5_SOURCE_DIR
    const auto source_path = std::filesystem::path(IME_FCITX5_SOURCE_DIR) / ".." / "tables";
    if (std::filesystem::exists(source_path / "tokens")) return source_path;
#endif

    return project_root_from_source() / "tables";
}

std::unordered_map<std::string, int> load_token_map(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("failed to open token table: " + path.string());
    return nlohmann::json::parse(input).get<std::unordered_map<std::string, int>>();
}

bool is_alpha(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool is_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

bool is_latin_char(char32_t c) {
    return is_alpha(c) || is_digit(c) || c == U'-' || c == U'_' || c == U'+';
}

char to_lower_ascii(char32_t c) {
    return static_cast<char>((c >= U'A' && c <= U'Z') ? (c ^ 0x20) : c);
}

class TokenTables {
public:
    explicit TokenTables(const std::filesystem::path& token_dir)
        : char_table_(load_token_map(token_dir / "chars.json")),
          latin_table_(load_token_map(token_dir / "latin.json")),
          special_table_(load_token_map(token_dir / "special_tokens.json")),
          bopomofo_table_(load_token_map(token_dir / "bpmf.json")) {}

    int map_char(char32_t c) const {
        const auto text = char32_to_utf8(c);
        const auto it = char_table_.find(text);
        return it == char_table_.end() ? -1 : it->second;
    }

    std::vector<int> tokenize(const PredictRequest& request) const {
        std::vector<int> tokens;
        tokens.push_back(special("<BOS>"));

        std::vector<int> context_tokens;
        const auto context = utf8_to_u32(u16_to_utf8(request.context));
        for (size_t i = 0; i < context.size(); ++i) {
            const char32_t c = context[i];
            if (c == U' ') {
                context_tokens.push_back(special("<SP>"));
            } else if (const int mapped = map_char(c); mapped != -1) {
                context_tokens.push_back(mapped);
            } else if (is_latin_char(c)) {
                std::string word;
                for (; i < context.size(); ++i) {
                    if (!is_latin_char(context[i])) {
                        --i;
                        break;
                    }
                    word.push_back(to_lower_ascii(context[i]));
                }
                const auto it = latin_table_.find(word);
                context_tokens.push_back(it == latin_table_.end() ? special("<LATIN>") : it->second);
            } else {
                context_tokens.push_back(special("<UNK>"));
            }
        }
        remove_leading_unknown_context_tokens(context_tokens, special("<UNK>"));
        tokens.insert(tokens.end(), context_tokens.begin(), context_tokens.end());

        for (const auto& entry : request.padding) {
            if (entry.chosen) {
                const int mapped = map_char(entry.chosen_char);
                tokens.push_back(mapped == -1 ? special("<UNK>") : mapped);
            } else {
                const std::string key = "<" + u16_to_utf8(entry.bopomofo) + ">";
                const auto it = bopomofo_table_.find(key);
                tokens.push_back(it == bopomofo_table_.end() ? special("<UNK>") : it->second);
            }
        }

        tokens.push_back(special("<SEP>"));
        return tokens;
    }

private:
    int special(const std::string& token) const {
        return special_table_.at(token);
    }

    std::unordered_map<std::string, int> char_table_;
    std::unordered_map<std::string, int> latin_table_;
    std::unordered_map<std::string, int> special_table_;
    std::unordered_map<std::string, int> bopomofo_table_;
};

#if IME_FCITX5_HAS_LLAMA
void decode_tokens(llama_context* context, const std::vector<int>& tokens, llama_pos start_pos, bool logits_last) {
    if (tokens.empty()) return;
    if (tokens.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        throw std::runtime_error("llama batch is too large");
    }

    llama_batch batch = llama_batch_init(static_cast<int32_t>(tokens.size()), 0, 1);
    if (batch.token == nullptr || batch.pos == nullptr || batch.n_seq_id == nullptr || batch.seq_id == nullptr ||
        batch.logits == nullptr) {
        llama_batch_free(batch);
        throw std::runtime_error("llama_batch_init failed");
    }

    batch.n_tokens = static_cast<int32_t>(tokens.size());
    for (int32_t i = 0; i < batch.n_tokens; ++i) {
        batch.token[i] = static_cast<llama_token>(tokens[static_cast<size_t>(i)]);
        batch.pos[i] = start_pos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = logits_last && i == batch.n_tokens - 1 ? 1 : 0;
    }

    const int rc = llama_decode(context, batch);
    llama_synchronize(context);
    llama_batch_free(batch);
    if (rc != 0) throw std::runtime_error("llama_decode failed");
}

struct ScoredCandidate {
    char32_t candidate = 0;
    int token = -1;
    float logit = 0.0f;
};
#endif

}  // namespace

struct LlamaBackend::Impl {
    bool ready = false;

#if IME_FCITX5_HAS_LLAMA
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    llama_memory_t memory = nullptr;
    const llama_vocab* vocab = nullptr;
    std::unique_ptr<TokenTables> tokens;
    std::unique_ptr<TableEngine> table;
    std::vector<int> prev_tokens;
    llama_pos next_pos = 0;

    ~Impl() {
        reset();
    }

    void reset() {
        if (context != nullptr) llama_free(context);
        if (model != nullptr) llama_model_free(model);
        context = nullptr;
        model = nullptr;
        memory = nullptr;
        vocab = nullptr;
        tokens.reset();
        table.reset();
        prev_tokens.clear();
        next_pos = 0;
        ready = false;
    }

    void clear_cache() {
        if (memory != nullptr) llama_memory_clear(memory, true);
        prev_tokens.clear();
        next_pos = 0;
    }

    void decode_and_cache(const std::vector<int>& tokens_to_decode) {
        if (tokens_to_decode.empty()) return;
        decode_tokens(context, tokens_to_decode, next_pos, true);
        next_pos += static_cast<llama_pos>(tokens_to_decode.size());
        prev_tokens.insert(prev_tokens.end(), tokens_to_decode.begin(), tokens_to_decode.end());
    }

    void decode_and_cache(int token) {
        decode_tokens(context, {token}, next_pos, true);
        ++next_pos;
        prev_tokens.push_back(token);
    }

    void ensure_cache_aligned(const std::vector<int>& new_tokens) {
        size_t common = 0;
        while (common < prev_tokens.size() && common < new_tokens.size() &&
               prev_tokens[common] == new_tokens[common]) {
            ++common;
        }

        if (common < prev_tokens.size()) {
            if (common == 0) {
                clear_cache();
            } else if (llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(common), -1)) {
                prev_tokens.resize(common);
            } else {
                clear_cache();
                common = 0;
            }
        }
        next_pos = static_cast<llama_pos>(common);

        if (common < new_tokens.size()) {
            std::vector<int> tail(new_tokens.begin() + static_cast<std::ptrdiff_t>(common),
                                  new_tokens.end());
            decode_and_cache(tail);
        }
    }
#else
    void reset() {
        ready = false;
    }
#endif
};

LlamaBackend::LlamaBackend() : impl_(std::make_unique<Impl>()) {}
LlamaBackend::~LlamaBackend() = default;
LlamaBackend::LlamaBackend(LlamaBackend&&) noexcept = default;
LlamaBackend& LlamaBackend::operator=(LlamaBackend&&) noexcept = default;

void LlamaBackend::load(const Config& cfg) {
    impl_->reset();

    if (cfg.model_path.empty()) throw std::runtime_error("llama.cpp model path is not configured");

#if !IME_FCITX5_HAS_LLAMA
    throw std::runtime_error("llama.cpp backend is unavailable in this build for model: " + cfg.model_path);
#else
    if (!std::filesystem::exists(cfg.model_path)) {
        throw std::runtime_error("llama.cpp model not found: " + cfg.model_path);
    }

    try {
        llama_backend_init();

        auto model_params = llama_model_default_params();
        model_params.n_gpu_layers = cfg.gpu_layers;

        impl_->model = llama_model_load_from_file(cfg.model_path.c_str(), model_params);
        if (impl_->model == nullptr) throw std::runtime_error("failed to load llama.cpp model: " + cfg.model_path);
        impl_->vocab = llama_model_get_vocab(impl_->model);

        auto context_params = llama_context_default_params();
        context_params.n_ctx = static_cast<std::uint32_t>(cfg.context_length);
        context_params.n_threads = cfg.thread_count;
        context_params.n_threads_batch = cfg.thread_count;

        impl_->context = llama_init_from_model(impl_->model, context_params);
        if (impl_->context == nullptr)
            throw std::runtime_error("failed to create llama.cpp context: " + cfg.model_path);
        impl_->memory = llama_get_memory(impl_->context);
        impl_->clear_cache();

        const auto table_dir = tables_dir();
        impl_->tokens = std::make_unique<TokenTables>(table_dir / "tokens");
        impl_->table = std::make_unique<TableEngine>(table_dir / "bopomofo_char.json");
        impl_->ready = true;
    } catch (...) {
        impl_->reset();
        throw;
    }
#endif
}

bool LlamaBackend::ready() const noexcept {
    return impl_->ready;
}

PredictResponse LlamaBackend::predict(const PredictRequest& request) {
    if (!ready()) throw std::runtime_error("llama.cpp backend is not ready");
#if IME_FCITX5_HAS_LLAMA
    const auto prompt_tokens = impl_->tokens->tokenize(request);
    impl_->ensure_cache_aligned(prompt_tokens);

    PredictResponse response;
    response.candidates.reserve(request.padding.size());

    for (const auto& entry : request.padding) {
        if (entry.chosen) {
            response.candidates.push_back({entry.chosen_char});
            if (const int token = impl_->tokens->map_char(entry.chosen_char); token != -1) {
                impl_->decode_and_cache(token);
            }
            continue;
        }

        float* logits = llama_get_logits_ith(impl_->context, -1);
        if (logits == nullptr) throw std::runtime_error("llama logits are unavailable");
        const int32_t vocab_size = llama_vocab_n_tokens(impl_->vocab);

        std::vector<ScoredCandidate> scored;
        for (const char32_t candidate : impl_->table->lookup(entry.bopomofo)) {
            const int token = impl_->tokens->map_char(candidate);
            if (token < 0 || token >= vocab_size) continue;
            scored.push_back({candidate, token, logits[token]});
        }

        std::sort(scored.begin(), scored.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
            return lhs.logit > rhs.logit;
        });

        std::vector<char32_t> candidates;
        candidates.reserve(scored.size());
        for (const auto& item : scored) candidates.push_back(item.candidate);
        if (!scored.empty()) impl_->decode_and_cache(scored.front().token);
        response.candidates.push_back(std::move(candidates));
    }

    return response;
#else
    (void)request;
    throw std::runtime_error("llama.cpp prediction bridge is unavailable in this build");
#endif
}

}  // namespace ime::fcitx5
