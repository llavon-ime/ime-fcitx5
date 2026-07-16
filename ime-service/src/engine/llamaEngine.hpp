#pragma once

#include <ggml-backend.h>
#include <llama-cpp.h>
#include <utf8/cpp20.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../core/bopomofo.hpp"
#include "../utils/runtime_paths.hpp"
#include "engine.h"
#include "model_runtime.hpp"

#ifndef IMESVC_TRACE_PREDICT
#define IMESVC_TRACE_PREDICT 0
#endif

#ifndef IMESVC_LOG_TIMING
#define IMESVC_LOG_TIMING 0
#endif

namespace imesvc {

inline constexpr int kLlamaThreads = 8;
inline constexpr auto kLlamaReadyIdleThreshold = std::chrono::milliseconds(500);
inline constexpr long long kSlowDecodeLogThresholdUs = 50'000;

struct LlamaOffloadDevice {
    ggml_backend_dev_t device = nullptr;
    enum ggml_backend_dev_type type = GGML_BACKEND_DEVICE_TYPE_CPU;
    size_t memory_free = 0;
    size_t memory_total = 0;
};

class ModelManager {
    static const char* device_type_name(enum ggml_backend_dev_type type) {
        switch (type) {
            case GGML_BACKEND_DEVICE_TYPE_CPU:
                return "CPU";
            case GGML_BACKEND_DEVICE_TYPE_GPU:
                return "GPU";
            case GGML_BACKEND_DEVICE_TYPE_IGPU:
                return "IGPU";
            case GGML_BACKEND_DEVICE_TYPE_ACCEL:
                return "ACCEL";
            case GGML_BACKEND_DEVICE_TYPE_META:
                return "META";
            default:
                return "UNKNOWN";
        }
    }

    static double mib(size_t bytes) {
        return static_cast<double>(bytes) / 1024.0 / 1024.0;
    }

    static LlamaOffloadDevice select_gpu_device() {
        const size_t count = ggml_backend_dev_count();
        std::vector<LlamaOffloadDevice> candidates;
        for (size_t i = 0; i < count; ++i) {
            ggml_backend_dev_t device = ggml_backend_dev_get(i);
            ggml_backend_dev_props props{};
            ggml_backend_dev_get_props(device, &props);
            const auto type = props.type;
            const char* name = props.name ? props.name : ggml_backend_dev_name(device);
            const char* description = props.description ? props.description : ggml_backend_dev_description(device);
            std::clog << "[SRV] llama device[" << i << "] type=" << device_type_name(type)
                      << " name=" << (name ? name : "<unknown>")
                      << " desc=" << (description ? description : "<unknown>")
                      << " free_mib=" << std::fixed << std::setprecision(1) << mib(props.memory_free)
                      << " total_mib=" << mib(props.memory_total) << std::defaultfloat
                      << " id=" << (props.device_id ? props.device_id : "<unknown>") << '\n';

            if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
                candidates.push_back({device, type, props.memory_free, props.memory_total});
            }
        }

        if (candidates.empty()) return {};

        std::sort(candidates.begin(), candidates.end(), [](const LlamaOffloadDevice& a, const LlamaOffloadDevice& b) {
            if (a.type != b.type) {
                return a.type == GGML_BACKEND_DEVICE_TYPE_IGPU;
            }
            if (a.memory_total != b.memory_total) return a.memory_total > b.memory_total;
            return a.memory_free > b.memory_free;
        });

        const auto selected = candidates.front();
        std::clog << "[SRV] llama selected offload device type=" << device_type_name(selected.type)
                  << " name=" << (ggml_backend_dev_name(selected.device) ? ggml_backend_dev_name(selected.device)
                                                                         : "<unknown>")
                  << " total_mib=" << std::fixed << std::setprecision(1) << mib(selected.memory_total)
                  << std::defaultfloat << '\n';
        return selected;
    }

    llama_model_ptr _model;
    const llama_vocab* _vocab;

    ModelManager() {
        llama_backend_init();
        auto path = RuntimePaths::model_path().string();
        auto model_params = llama_model_default_params();
        std::array<ggml_backend_dev_t, 2> offload_devices{};
        LlamaOffloadDevice offload_device = select_gpu_device();
        const bool supports_gpu_offload = llama_supports_gpu_offload();
        const int requested_gpu_layers = RuntimePaths::gpu_layers();
        const bool wants_gpu = requested_gpu_layers != 0 &&
                               (requested_gpu_layers == -2 || requested_gpu_layers == -1 || requested_gpu_layers > 0);
        const bool use_gpu_offload = wants_gpu && offload_device.device && supports_gpu_offload;
        if (use_gpu_offload) {
            offload_devices[0] = offload_device.device;
            offload_devices[1] = nullptr;
            model_params.devices = offload_devices.data();
            model_params.n_gpu_layers = requested_gpu_layers == -2 || requested_gpu_layers == -1 ? -1 : requested_gpu_layers;
            model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
            model_params.main_gpu = 0;
        }

        std::clog << "[SRV] loading model: " << path << '\n';
        std::clog << "[SRV] llama gpu_offload=" << (supports_gpu_offload ? "supported" : "unavailable")
                  << " gpu_layers=" << model_params.n_gpu_layers << " main_gpu=" << model_params.main_gpu
                  << '\n';
        std::clog << "[SRV] llama offload=" << (use_gpu_offload ? "enabled" : "disabled") << '\n';
        _model.reset(llama_model_load_from_file(path.c_str(), model_params));
        if (!_model) throw std::runtime_error("Failed to load model: " + path);
        _vocab = llama_model_get_vocab(_model.get());
        std::clog << "[SRV] model loaded\n";
    }

public:
    static void initialize() {
        (void)instance();
    }
    static ModelManager& instance() {
        static ModelManager e;
        return e;
    }
    llama_model* model() {
        return _model.get();
    }
    const llama_vocab* vocab() {
        return _vocab;
    }
    llama_context* new_context(uint32_t n_ctx = 0, uint32_t n_batch = 0) {
        std::clog << "[SRV] creating context\n";
        auto params = llama_context_default_params();
        if (n_ctx != 0) {
            params.n_ctx = n_ctx;
        } else if (RuntimePaths::context_length() != 0) {
            params.n_ctx = RuntimePaths::context_length();
        }
        if (n_batch != 0) {
            params.n_batch = n_batch;
            params.n_ubatch = n_batch;
        }
        params.n_threads = static_cast<int32_t>(RuntimePaths::threads());
        params.n_threads_batch = static_cast<int32_t>(RuntimePaths::threads());
        auto ctx = llama_init_from_model(_model.get(), params);
        if (!ctx) throw std::runtime_error("Failed to create llama context");
        std::clog << "[SRV] context created threads=" << RuntimePaths::threads() << '\n';
        return ctx;
    }
};

class LlamaEngine : public IEngine {
    llama_context_ptr llama_ctx;
    llama_context_ptr warmup_ctx;
    struct AdapterDeleter {
        void operator()(llama_adapter_lora* adapter) const noexcept {
            if (adapter != nullptr) llama_adapter_lora_free(adapter);
        }
    };
    std::shared_ptr<const AdapterGeneration> adapter_generation_;
    std::unique_ptr<llama_adapter_lora, AdapterDeleter> adapter_;
    std::vector<llama_token> prev_tokens;
    llama_memory_t mem;
    llama_pos next_pos = 0;
    std::chrono::steady_clock::time_point last_backend_touch = std::chrono::steady_clock::time_point::min();

    struct PredictTiming {
        long long tokenize_us = 0;
        long long cache_us = 0;
        long long cache_batch_us = 0;
        long long cache_decode_us = 0;
        long long cache_sync_us = 0;
        long long candidate_us = 0;
        long long mask_us = 0;
        long long step_batch_us = 0;
        long long step_decode_us = 0;
        long long step_sync_us = 0;
        long long log_us = 0;
        size_t cache_tokens = 0;
        size_t candidate_tokens = 0;
        size_t mask_calls = 0;
        size_t decode_calls = 0;
    };

public:
    explicit LlamaEngine(std::shared_ptr<const AdapterGeneration> adapter_generation = {})
        : adapter_generation_(std::move(adapter_generation)) {
        ModelManager::initialize();
        if (adapter_generation_) {
            adapter_.reset(llama_adapter_lora_init(ModelManager::instance().model(), adapter_generation_->path.c_str()));
            if (!adapter_) throw std::runtime_error("llama.cpp rejected the active LoRA adapter");
        }
        llama_ctx.reset(ModelManager::instance().new_context());
        attach_adapter(llama_ctx.get());
        mem = llama_get_memory(llama_ctx.get());
        llama_memory_clear(mem, true);
        std::clog << "[SRV] engine ready\n";
    }

    static void validate_adapter(const std::filesystem::path& path) {
        ModelManager::initialize();
        std::unique_ptr<llama_adapter_lora, AdapterDeleter> adapter(
            llama_adapter_lora_init(ModelManager::instance().model(), path.c_str()));
        if (!adapter) throw std::runtime_error("llama.cpp rejected the GGUF LoRA adapter");
    }

    void ready() override {
        const auto now = std::chrono::steady_clock::now();
        if (last_backend_touch != std::chrono::steady_clock::time_point::min() &&
            now - last_backend_touch < kLlamaReadyIdleThreshold) {
            return;
        }

        if (!warmup_ctx) {
            warmup_ctx.reset(ModelManager::instance().new_context(8, 1));
            attach_adapter(warmup_ctx.get());
        }

        llama_token token = warmup_token();
        llama_set_warmup(warmup_ctx.get(), true);
        llama_batch batch = make_token_batch(&token, 1, 0, false);
#if IMESVC_LOG_TIMING
        const auto warmup_start = std::chrono::steady_clock::now();
#endif
        int rc = llama_decode(warmup_ctx.get(), batch);
        llama_synchronize(warmup_ctx.get());
        llama_batch_free(batch);
        llama_memory_clear(llama_get_memory(warmup_ctx.get()), true);
        llama_set_warmup(warmup_ctx.get(), false);
        if (rc == 0) mark_backend_touch();
#if IMESVC_LOG_TIMING
        const auto warmup_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - warmup_start)
                .count();
        std::clog << "[TIME] ready_warmup_ms=" << warmup_ms << '\n';
#endif
        if (rc != 0) throw std::runtime_error("llama_decode failed in ready warmup");
    }

    std::vector<PredictResult> predict(const std::u16string& context,
                                       const std::vector<PaddingEntry>& padding) override {
#if IMESVC_LOG_TIMING
        const auto predict_start = std::chrono::steady_clock::now();
#endif
        PredictTiming timing;

        const auto tokenize_start = std::chrono::steady_clock::now();
        auto& tok = Tokenizer::instance();
        std::vector<int> new_tokens = tok.tokenize(context, padding);
        if (padding.size() > 191U) {
            throw std::runtime_error("prediction readings exceed the native 384-token model window");
        }
        const auto maximum_prompt_tokens = 384U - padding.size();
        if (new_tokens.size() > maximum_prompt_tokens) {
            const auto context_tokens = new_tokens.size() - padding.size() - 2U;
            const auto excess = new_tokens.size() - maximum_prompt_tokens;
            if (excess > context_tokens) {
                throw std::runtime_error("prediction readings exceed the native 384-token model window");
            }
            // Keep BOS, every reading, SEP, and room for one generated character per reading.
            new_tokens.erase(new_tokens.begin() + 1,
                             new_tokens.begin() + 1 + static_cast<std::ptrdiff_t>(excess));
        }
        timing.tokenize_us += elapsed_us(tokenize_start);

#if IMESVC_TRACE_PREDICT
        const auto request_log_start = std::chrono::steady_clock::now();
        debug_request(context, padding, new_tokens);

        std::clog << "[SRV] predict: ctx_len=" << context.size() << " pad_cnt=" << padding.size()
                  << " tokens=" << new_tokens.size() << '\n';
        timing.log_us += elapsed_us(request_log_start);
#endif

        const auto cache_start = std::chrono::steady_clock::now();
        ensure_cache_aligned(new_tokens, timing);
        timing.cache_us += elapsed_us(cache_start);

        std::vector<PredictResult> results;
        results.reserve(padding.size());

        for (size_t pi = 0; pi < padding.size(); pi++) {
            auto& entry = padding[pi];
            PredictResult r;
            if (!entry.is_chosen) {
                const auto candidate_start = std::chrono::steady_clock::now();
                auto candidates = HanziMapEngine::instance().lookup_all(entry.bpmf);
                if (!candidates.empty()) {
                    std::vector<llama_token> cand_tokens;
                    std::map<llama_token, char32_t> inv;
                    for (auto c : candidates) {
                        auto t = tok.map_char(c);
                        if (t != -1) {
                            cand_tokens.push_back(t);
                            inv[t] = c;
                        }
                    }
                    timing.candidate_us += elapsed_us(candidate_start);
                    timing.candidate_tokens += cand_tokens.size();
                    if (!cand_tokens.empty()) {
                        auto probs = masked_predict(cand_tokens, timing);
                        for (auto& [token, prob] : probs) r.candidates.push_back({inv[token], prob});

#if IMESVC_TRACE_PREDICT
                        const auto top_log_start = std::chrono::steady_clock::now();
                        debug_top5(pi, entry, probs, inv);
                        timing.log_us += elapsed_us(top_log_start);
#endif

                        llama_token best = static_cast<llama_token>(probs.front().token);
                        decode_one(best, timing);
                    }
                } else {
                    timing.candidate_us += elapsed_us(candidate_start);
#if IMESVC_TRACE_PREDICT
                    const auto no_candidate_log_start = std::chrono::steady_clock::now();
                    debug_no_candidates(pi, entry);
                    timing.log_us += elapsed_us(no_candidate_log_start);
#endif
                }
            } else {
                const auto candidate_start = std::chrono::steady_clock::now();
                auto t = tok.map_char(entry.chosen_char);
                timing.candidate_us += elapsed_us(candidate_start);
                timing.candidate_tokens += (t == -1) ? 0 : 1;

#if IMESVC_TRACE_PREDICT
                const auto chosen_log_start = std::chrono::steady_clock::now();
                debug_chosen(pi, entry, t);
                timing.log_us += elapsed_us(chosen_log_start);
#endif
                if (t != -1) decode_one(t, timing);
                if (t != -1) r.candidates.push_back({entry.chosen_char, 1.0F});
            }
            results.push_back(std::move(r));
        }

#if IMESVC_LOG_TIMING
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - predict_start)
                .count();
        std::clog << "[TIME] predict_ms=" << elapsed_ms << '\n';
        print_timing(timing);
#endif
#if IMESVC_TRACE_PREDICT
        std::clog << "[SRV] predict done\n";
#endif
        return results;
    }

private:
    void attach_adapter(llama_context* context) const {
        if (!adapter_) return;
        llama_adapter_lora* adapters[] = {adapter_.get()};
        float scales[] = {1.0F};
        if (llama_set_adapters_lora(context, adapters, 1, scales) != 0) {
            throw std::runtime_error("llama.cpp failed to attach the active LoRA adapter");
        }
    }

    struct TokenProb {
        int token;
        float prob;
    };

    static long long elapsed_us(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    }

    static double ms(long long us) {
        return static_cast<double>(us) / 1000.0;
    }

    static llama_token warmup_token() {
        const llama_vocab* vocab = ModelManager::instance().vocab();
        const llama_token candidates[] = {
            llama_vocab_bos(vocab),
            llama_vocab_eos(vocab),
            llama_vocab_nl(vocab),
            llama_vocab_pad(vocab),
        };
        for (llama_token token : candidates) {
            if (token >= 0) return token;
        }
        return 0;
    }

    void mark_backend_touch() {
        last_backend_touch = std::chrono::steady_clock::now();
    }

    static void print_timing(const PredictTiming& timing) {
        std::clog << std::fixed << std::setprecision(3) << "[TIME] detail_ms"
                  << " tokenize=" << ms(timing.tokenize_us) << " cache_total=" << ms(timing.cache_us)
                  << " cache_batch=" << ms(timing.cache_batch_us) << " cache_decode=" << ms(timing.cache_decode_us)
                  << " cache_sync=" << ms(timing.cache_sync_us)
                  << " candidate=" << ms(timing.candidate_us) << " mask=" << ms(timing.mask_us)
                  << " step_batch=" << ms(timing.step_batch_us) << " step_decode=" << ms(timing.step_decode_us)
                  << " step_sync=" << ms(timing.step_sync_us)
                  << " log=" << ms(timing.log_us) << " cache_tokens=" << timing.cache_tokens
                  << " candidate_tokens=" << timing.candidate_tokens << " mask_calls=" << timing.mask_calls
                  << " decode_calls=" << timing.decode_calls << std::defaultfloat << '\n';
    }

    static void log_slow_decode(const char* stage, llama_pos pos, size_t token_count, llama_token last_token,
                                long long decode_us, long long sync_us) {
        const long long total_us = decode_us + sync_us;
        if (total_us < kSlowDecodeLogThresholdUs) return;

        std::clog << std::fixed << std::setprecision(3) << "[TIME] slow_decode stage=" << stage
                  << " pos=" << pos << " tokens=" << token_count << " last_token=" << last_token
                  << " decode_ms=" << ms(decode_us) << " sync_ms=" << ms(sync_us)
                  << " total_ms=" << ms(total_us) << std::defaultfloat << '\n';
    }

#if IMESVC_TRACE_PREDICT
    static std::string to_utf8(const std::u16string& text) {
        return utf8::utf16to8(text);
    }

    static std::string to_utf8(char32_t ch) {
        std::string text;
        utf8::append(ch, text);
        return text;
    }

    static std::string describe_padding(const PaddingEntry& entry) {
        if (entry.is_chosen) return to_utf8(entry.chosen_char);
        return "<" + to_utf8(entry.bpmf) + ">";
    }

    static void debug_request(const std::u16string& context, const std::vector<PaddingEntry>& padding,
                               const std::vector<int>& tokens) {
        // Trace output must not expose locally typed text or candidate selections.
        std::clog << "[REQ] context_units=" << context.size() << " padding=" << padding.size()
                  << " tokens=" << tokens.size() << '\n';
    }

    static void debug_top5(size_t pos, const PaddingEntry& entry, const std::vector<TokenProb>& probs,
                           const std::map<llama_token, char32_t>& inv) {
        (void)entry;
        (void)inv;
        std::clog << "[POS " << pos << "] bpmf_units=" << entry.bpmf.size() << " top5=";
        const size_t count = std::min<size_t>(5, probs.size());
        for (size_t i = 0; i < count; ++i) {
            const auto token = static_cast<llama_token>(probs[i].token);
            if (i != 0) std::clog << ", ";
            std::clog << "token=" << token << ", p=" << std::fixed << std::setprecision(6) << probs[i].prob;
        }
        std::clog << std::defaultfloat << '\n';
    }

    static void debug_no_candidates(size_t pos, const PaddingEntry& entry) {
        (void)entry;
        std::clog << "[POS " << pos << "] top5=<no candidates>\n";
    }

    static void debug_chosen(size_t pos, const PaddingEntry& entry, int token) {
        (void)entry;
        std::clog << "[POS " << pos << "] chosen token=" << token << '\n';
    }
#else
    static void debug_request(const std::u16string&, const std::vector<PaddingEntry>&, const std::vector<int>&) {}

    static void debug_top5(size_t, const PaddingEntry&, const std::vector<TokenProb>&,
                           const std::map<llama_token, char32_t>&) {}

    static void debug_no_candidates(size_t, const PaddingEntry&) {}

    static void debug_chosen(size_t, const PaddingEntry&, int) {}
#endif

    static llama_batch make_token_batch(const llama_token* tokens, size_t count, llama_pos start_pos,
                                        bool logits_last) {
        if (count == 0 || count > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            throw std::runtime_error("invalid llama batch size");
        }

        llama_batch batch = llama_batch_init(static_cast<int32_t>(count), 0, 1);
        if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id || !batch.logits) {
            llama_batch_free(batch);
            throw std::runtime_error("llama_batch_init failed");
        }

        batch.n_tokens = static_cast<int32_t>(count);
        for (int32_t i = 0; i < batch.n_tokens; ++i) {
            batch.token[i] = tokens[i];
            batch.pos[i] = start_pos + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = (!logits_last || i == batch.n_tokens - 1) ? 1 : 0;
        }
        return batch;
    }

    void decode_one(llama_token token, PredictTiming& timing) {
        const auto batch_start = std::chrono::steady_clock::now();
        llama_batch batch = make_token_batch(&token, 1, next_pos, true);
        timing.step_batch_us += elapsed_us(batch_start);

        const auto decode_start = std::chrono::steady_clock::now();
        int rc = llama_decode(llama_ctx.get(), batch);
        const long long decode_us = elapsed_us(decode_start);
        if (rc == 0) mark_backend_touch();
        timing.step_decode_us += decode_us;

        const auto sync_start = std::chrono::steady_clock::now();
        llama_synchronize(llama_ctx.get());
        const long long sync_us = elapsed_us(sync_start);
        timing.step_sync_us += sync_us;
        log_slow_decode("step", next_pos, 1, token, decode_us, sync_us);

        const auto free_start = std::chrono::steady_clock::now();
        llama_batch_free(batch);
        timing.step_batch_us += elapsed_us(free_start);
        timing.decode_calls++;
        if (rc != 0) throw std::runtime_error("llama_decode failed in decode_one");

        ++next_pos;
        prev_tokens.push_back(token);
    }

    void ensure_cache_aligned(const std::vector<int>& new_tokens, PredictTiming& timing) {
        size_t common = 0;
        while (common < prev_tokens.size() && common < new_tokens.size() && prev_tokens[common] == new_tokens[common]) {
            common++;
        }

#if IMESVC_TRACE_PREDICT
        std::clog << "[SRV] cache: prev=" << prev_tokens.size() << " new=" << new_tokens.size() << " common=" << common
                  << '\n';
#endif

        if (common < prev_tokens.size()) {
            bool ok = llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1);
#if IMESVC_TRACE_PREDICT
            std::clog << "[SRV] seq_rm from " << common << " -> " << (ok ? "ok" : "FAIL") << '\n';
#else
            (void)ok;
#endif
            prev_tokens.resize(common);
        }
        next_pos = static_cast<llama_pos>(common);

        size_t new_count = new_tokens.size() - common;
        if (new_count > 0) {
            std::vector<llama_token> prompt_tokens;
            prompt_tokens.reserve(new_count);
            for (size_t i = 0; i < new_count; ++i) {
                prompt_tokens.push_back(static_cast<llama_token>(new_tokens[common + i]));
            }

            const auto batch_start = std::chrono::steady_clock::now();
            llama_batch batch = make_token_batch(prompt_tokens.data(), prompt_tokens.size(), next_pos, true);
            timing.cache_batch_us += elapsed_us(batch_start);

            const auto decode_start = std::chrono::steady_clock::now();
            int rc = llama_decode(llama_ctx.get(), batch);
            const long long decode_us = elapsed_us(decode_start);
            if (rc == 0) mark_backend_touch();
            timing.cache_decode_us += decode_us;

            const auto sync_start = std::chrono::steady_clock::now();
            llama_synchronize(llama_ctx.get());
            const long long sync_us = elapsed_us(sync_start);
            timing.cache_sync_us += sync_us;
            log_slow_decode("cache", next_pos, prompt_tokens.size(), prompt_tokens.back(), decode_us, sync_us);

            const auto free_start = std::chrono::steady_clock::now();
            llama_batch_free(batch);
            timing.cache_batch_us += elapsed_us(free_start);
            timing.cache_tokens += new_count;
            if (rc != 0) throw std::runtime_error("llama_decode failed in ensure_cache");

            next_pos += static_cast<llama_pos>(new_count);
            prev_tokens.insert(prev_tokens.end(), new_tokens.begin() + common, new_tokens.end());
        }
    }

    std::vector<TokenProb> masked_predict(const std::vector<llama_token>& candidate, PredictTiming& timing) {
        const auto mask_start = std::chrono::steady_clock::now();
        float* logits = llama_get_logits_ith(llama_ctx.get(), -1);
        if (!logits) throw std::runtime_error("llama_get_logits_ith error");

        std::vector<float> cand_logits(candidate.size());
        for (size_t i = 0; i < candidate.size(); i++) cand_logits[i] = logits[candidate[i]];

        float max_logit = *std::ranges::max_element(cand_logits);
        std::vector<float> exps(candidate.size());
        float sum = 0.0f;
        for (size_t i = 0; i < candidate.size(); ++i) {
            exps[i] = std::exp(cand_logits[i] - max_logit);
            sum += exps[i];
        }

        std::vector<TokenProb> probs;
        probs.reserve(candidate.size());
        for (size_t i = 0; i < candidate.size(); ++i) probs.push_back({static_cast<int>(candidate[i]), exps[i] / sum});

        std::sort(probs.begin(), probs.end(), [](const TokenProb& a, const TokenProb& b) {
            return a.prob > b.prob;
        });
        timing.mask_us += elapsed_us(mask_start);
        timing.mask_calls++;
        return probs;
    }
};

}  // namespace imesvc
