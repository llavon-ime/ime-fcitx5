#pragma once

#include <ggml-backend.h>
#include <llama-cpp.h>
#include <utf8/cpp20.h>

#include <ime-core/logger.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../bopomofo/bopomofo.hpp"
#include "engine.hpp"
#include "../utils/core_paths.hpp"

#ifndef IME_CORE_TRACE_PREDICT
#define IME_CORE_TRACE_PREDICT 0
#endif

namespace llavon::ime::core::internal {

inline constexpr auto kLlamaReadyIdleThreshold = std::chrono::milliseconds(500);
inline constexpr long long kSlowDecodeLogThresholdUs = 50'000;

struct LlamaOffloadDevice {
    ggml_backend_dev_t device = nullptr;
    enum ggml_backend_dev_type type = GGML_BACKEND_DEVICE_TYPE_CPU;
    InferenceBackend backend = InferenceBackend::cpu;
    std::string device_id;
    size_t memory_free = 0;
    size_t memory_total = 0;
};

class ModelManager {
public:
    static std::vector<InferenceDeviceInfo> enumerate_devices() {
        ensure_backend_initialized();
        std::vector<InferenceDeviceInfo> result;
        for (const auto& device : scan_devices()) {
            result.push_back(public_device_info(device));
        }
        return result;
    }

    const InferenceRuntimeInfo& runtime_info() const noexcept {
        return runtime_info_;
    }

private:
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

    static void ensure_backend_initialized() {
        static std::once_flag once;
        std::call_once(once, [] {
            // Dynamic ggml builds load the backend DLLs from the executable
            // directory. Loading backends does not load a model.
            ggml_backend_load_all();
            llama_backend_init();
        });
    }

    static std::string lowercase(const char* value) {
        std::string result = value ? value : "";
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }

    static std::optional<InferenceBackend> backend_type(ggml_backend_dev_t device,
                                                         enum ggml_backend_dev_type type) {
        if (type == GGML_BACKEND_DEVICE_TYPE_CPU) {
            return InferenceBackend::cpu;
        }

        const ggml_backend_reg_t registry = ggml_backend_dev_backend_reg(device);
        const std::string registry_name = lowercase(registry ? ggml_backend_reg_name(registry) : nullptr);
        if (registry_name.find("cuda") != std::string::npos) {
            return InferenceBackend::cuda;
        }
        if (registry_name.find("vulkan") != std::string::npos) {
            return InferenceBackend::vulkan;
        }
        if (registry_name.find("metal") != std::string::npos) {
            return InferenceBackend::metal;
        }
        return std::nullopt;
    }

    static InferenceDeviceType public_device_type(enum ggml_backend_dev_type type) {
        if (type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            return InferenceDeviceType::integrated_gpu;
        }
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU) {
            return InferenceDeviceType::gpu;
        }
        return InferenceDeviceType::cpu;
    }

    static InferenceDeviceInfo public_device_info(const LlamaOffloadDevice& device) {
        ggml_backend_dev_props props{};
        ggml_backend_dev_get_props(device.device, &props);
        const char* name = props.name ? props.name : ggml_backend_dev_name(device.device);
        const char* description =
            props.description ? props.description : ggml_backend_dev_description(device.device);
        return InferenceDeviceInfo{
            .backend = device.backend,
            .type = public_device_type(device.type),
            .device_id = device.device_id,
            .name = name ? name : "",
            .description = description ? description : "",
            .memory_free = static_cast<std::uint64_t>(device.memory_free),
            .memory_total = static_cast<std::uint64_t>(device.memory_total),
        };
    }

    static std::vector<LlamaOffloadDevice> scan_devices() {
        const size_t count = ggml_backend_dev_count();
        std::vector<LlamaOffloadDevice> devices;
        for (size_t i = 0; i < count; ++i) {
            ggml_backend_dev_t device = ggml_backend_dev_get(i);
            ggml_backend_dev_props props{};
            ggml_backend_dev_get_props(device, &props);
            const auto type = props.type;
            const auto backend = backend_type(device, type);
            const char* name = props.name ? props.name : ggml_backend_dev_name(device);
            const char* description = props.description ? props.description : ggml_backend_dev_description(device);
            std::clog << "[CORE] llama device[" << i << "] type=" << device_type_name(type)
                      << " name=" << (name ? name : "<unknown>")
                      << " desc=" << (description ? description : "<unknown>")
                      << " free_mib=" << std::fixed << std::setprecision(1) << mib(props.memory_free)
                      << " total_mib=" << mib(props.memory_total) << std::defaultfloat
                      << " id=" << (props.device_id ? props.device_id : "<unknown>") << '\n';

            if (backend) {
                const std::string device_id =
                    props.device_id && props.device_id[0] != '\0'
                        ? props.device_id
                        : (name ? name : "");
                devices.push_back(
                    {device, type, *backend, device_id, props.memory_free, props.memory_total});
            }
        }
        return devices;
    }

    static int backend_priority(InferenceBackend backend) {
        if (backend == InferenceBackend::cuda) return 0;
        if (backend == InferenceBackend::vulkan) return 1;
        if (backend == InferenceBackend::metal) return 2;
        return 3;
    }

    static const char* inference_backend_name(InferenceBackend backend) {
        if (backend == InferenceBackend::cpu) return "CPU";
        if (backend == InferenceBackend::cuda) return "CUDA";
        if (backend == InferenceBackend::vulkan) return "Vulkan";
        if (backend == InferenceBackend::metal) return "Metal";
        return "automatic";
    }

    static LlamaOffloadDevice select_gpu_device(const InferenceDeviceSelection& requested) {
        std::vector<LlamaOffloadDevice> candidates;
        for (const auto& device : scan_devices()) {
            if (device.type != GGML_BACKEND_DEVICE_TYPE_GPU &&
                device.type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
                continue;
            }
            if (requested.backend != InferenceBackend::automatic &&
                device.backend != requested.backend) {
                continue;
            }
            if (!requested.device_id.empty() && device.device_id != requested.device_id) {
                continue;
            }
            candidates.push_back(device);
        }

        if (candidates.empty()) return {};

        std::sort(candidates.begin(), candidates.end(), [](const LlamaOffloadDevice& a, const LlamaOffloadDevice& b) {
            if (a.type != b.type) {
                return a.type == GGML_BACKEND_DEVICE_TYPE_GPU;
            }
            if (a.backend != b.backend) return backend_priority(a.backend) < backend_priority(b.backend);
            if (a.memory_total != b.memory_total) return a.memory_total > b.memory_total;
            return a.memory_free > b.memory_free;
        });

        const auto selected = candidates.front();
        std::clog << "[CORE] llama selected offload device backend="
                  << inference_backend_name(selected.backend)
                  << " type=" << device_type_name(selected.type)
                  << " name=" << (ggml_backend_dev_name(selected.device) ? ggml_backend_dev_name(selected.device)
                                                                         : "<unknown>")
                  << " total_mib=" << std::fixed << std::setprecision(1) << mib(selected.memory_total)
                  << std::defaultfloat << '\n';
        return selected;
    }

    static LlamaOffloadDevice select_cpu_device() {
        for (const auto& device : scan_devices()) {
            if (device.backend == InferenceBackend::cpu &&
                device.type == GGML_BACKEND_DEVICE_TYPE_CPU) {
                return device;
            }
        }
        return {};
    }

    llama_model_ptr _model;
    const llama_vocab* _vocab;
    InferenceRuntimeInfo runtime_info_;

    ModelManager() {
        ensure_backend_initialized();
        auto path = CorePaths::model_path().string();
        auto model_params = llama_model_default_params();
        std::array<ggml_backend_dev_t, 2> offload_devices{};
        const auto& requested_device = CorePaths::inference_device();
        LlamaOffloadDevice offload_device =
            requested_device.backend == InferenceBackend::cpu
                ? LlamaOffloadDevice{}
                : select_gpu_device(requested_device);
        const bool supports_gpu_offload = llama_supports_gpu_offload();
        const int requested_gpu_layers = CorePaths::gpu_layers();
        const bool wants_gpu = requested_device.backend != InferenceBackend::cpu &&
                               requested_gpu_layers != 0 &&
                               (requested_gpu_layers == -2 || requested_gpu_layers == -1 || requested_gpu_layers > 0);
        const bool use_gpu_offload = wants_gpu && offload_device.device && supports_gpu_offload;
        if (use_gpu_offload) {
            offload_devices[0] = offload_device.device;
            offload_devices[1] = nullptr;
            model_params.devices = offload_devices.data();
            model_params.n_gpu_layers = requested_gpu_layers == -2 || requested_gpu_layers == -1 ? -1 : requested_gpu_layers;
            model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
            model_params.main_gpu = 0;
        } else {
            // llama.cpp defaults to using every available accelerator and to
            // offloading all layers. Supply an explicitly empty device list as
            // well as zero GPU layers so CPU mode (and GPU fallback) cannot
            // silently initialize or execute on an accelerator.
            model_params.devices = offload_devices.data();
            model_params.n_gpu_layers = 0;
        }

        std::clog << "[CORE] loading model: " << path << '\n';
        std::clog << "[CORE] requested inference backend="
                  << inference_backend_name(requested_device.backend)
                  << " device_id="
                  << (requested_device.device_id.empty() ? "<automatic>" : requested_device.device_id)
                  << '\n';
        std::clog << "[CORE] llama gpu_offload=" << (supports_gpu_offload ? "supported" : "unavailable")
                  << " gpu_layers=" << model_params.n_gpu_layers << " main_gpu=" << model_params.main_gpu
                  << '\n';
        std::clog << "[CORE] llama offload=" << (use_gpu_offload ? "enabled" : "disabled") << '\n';
        if (wants_gpu && requested_device.backend != InferenceBackend::automatic && !offload_device.device) {
            std::clog << "[CORE] requested inference device unavailable; falling back to CPU\n";
        }
        _model.reset(llama_model_load_from_file(path.c_str(), model_params));
        if (!_model) throw std::runtime_error("Failed to load model: " + path);
        _vocab = llama_model_get_vocab(_model.get());
        const LlamaOffloadDevice active_device =
            use_gpu_offload ? offload_device : select_cpu_device();
        if (!active_device.device) {
            throw std::runtime_error("Failed to identify the active llama inference device");
        }
        runtime_info_ = InferenceRuntimeInfo{
            .device = public_device_info(active_device),
            .gpu_offload = use_gpu_offload,
            .fell_back_to_cpu = wants_gpu && !use_gpu_offload,
        };
        std::clog << "[CORE] active inference device backend="
                  << inference_backend_name(runtime_info_.device.backend)
                  << " name=" << runtime_info_.device.name
                  << " id=" << runtime_info_.device.device_id << '\n';
        std::clog << "[CORE] model loaded\n";
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
        std::clog << "[CORE] creating context\n";
        auto params = llama_context_default_params();
        if (n_ctx != 0) {
            params.n_ctx = n_ctx;
        } else if (CorePaths::context_length() != 0) {
            params.n_ctx = CorePaths::context_length();
        }
        if (n_batch != 0) {
            params.n_batch = n_batch;
            params.n_ubatch = n_batch;
        }
        params.n_threads = static_cast<int32_t>(CorePaths::threads());
        params.n_threads_batch = static_cast<int32_t>(CorePaths::threads());
        auto ctx = llama_init_from_model(_model.get(), params);
        if (!ctx) throw std::runtime_error("Failed to create llama context");
        std::clog << "[CORE] context created threads=" << CorePaths::threads() << '\n';
        return ctx;
    }
};

class LlamaEngine : public IEngine {
    llama_context_ptr llama_ctx;
    llama_context_ptr warmup_ctx;
    std::vector<llama_token> prev_tokens;
    llama_memory_t mem;
    llama_pos next_pos = 0;
    std::chrono::steady_clock::time_point last_backend_touch = std::chrono::steady_clock::time_point::min();
    std::shared_ptr<Logger> logger_;

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
    explicit LlamaEngine(std::shared_ptr<Logger> logger) : logger_(std::move(logger)) {
        ModelManager::initialize();
        llama_ctx.reset(ModelManager::instance().new_context());
        mem = llama_get_memory(llama_ctx.get());
        llama_memory_clear(mem, true);
        logger_->log("[CORE] engine ready");
    }

    void ready() override {
        const auto now = std::chrono::steady_clock::now();
        if (last_backend_touch != std::chrono::steady_clock::time_point::min() &&
            now - last_backend_touch < kLlamaReadyIdleThreshold) {
            return;
        }

        if (!warmup_ctx) {
            warmup_ctx.reset(ModelManager::instance().new_context(8, 1));
        }

        llama_token token = warmup_token();
        llama_set_warmup(warmup_ctx.get(), true);
        llama_batch batch = make_token_batch(&token, 1, 0, false);
        const auto warmup_start = std::chrono::steady_clock::now();
        int rc = llama_decode(warmup_ctx.get(), batch);
        llama_synchronize(warmup_ctx.get());
        llama_batch_free(batch);
        llama_memory_clear(llama_get_memory(warmup_ctx.get()), true);
        llama_set_warmup(warmup_ctx.get(), false);
        if (rc == 0) mark_backend_touch();
        const auto warmup_us = elapsed_us(warmup_start);
        logger_->log(
            std::format("[TIME] ready_warmup_ms={:.3f}", milliseconds(warmup_us)));
        if (rc != 0) throw std::runtime_error("llama_decode failed in ready warmup");
    }

    std::vector<PredictResult> predict(const std::u16string& context,
                                       const std::vector<PaddingEntry>& padding) override {
        const auto predict_start = std::chrono::steady_clock::now();
        PredictTiming timing;

        const auto tokenize_start = std::chrono::steady_clock::now();
        auto& tok = Tokenizer::instance();
        std::vector<int> new_tokens = tok.tokenize(context, padding);
        timing.tokenize_us += elapsed_us(tokenize_start);

#if IME_CORE_TRACE_PREDICT
        const auto request_log_start = std::chrono::steady_clock::now();
        debug_request(context, padding, new_tokens);

        const auto context_length = context.size();
        const auto padding_count = padding.size();
        const auto token_count = new_tokens.size();
        logger_->log([context_length, padding_count, token_count] {
            return std::format("[CORE] predict ctx_len={} pad_cnt={} tokens={}",
                               context_length, padding_count, token_count);
        });
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

#if IME_CORE_TRACE_PREDICT
                        const auto top_log_start = std::chrono::steady_clock::now();
                        debug_top5(pi, entry, probs, inv);
                        timing.log_us += elapsed_us(top_log_start);
#endif

                        llama_token best = static_cast<llama_token>(probs.front().token);
                        decode_one(best, timing);
                    }
                } else {
                    timing.candidate_us += elapsed_us(candidate_start);
#if IME_CORE_TRACE_PREDICT
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

#if IME_CORE_TRACE_PREDICT
                const auto chosen_log_start = std::chrono::steady_clock::now();
                debug_chosen(pi, entry, t);
                timing.log_us += elapsed_us(chosen_log_start);
#endif
                if (t != -1) decode_one(t, timing);
            }
            results.push_back(std::move(r));
        }

        const auto total_us = elapsed_us(predict_start);
        logger_->log([timing, total_us] { return format_timing(total_us, timing); });
#if IME_CORE_TRACE_PREDICT
        logger_->log("[CORE] predict done");
#endif
        return results;
    }

private:
    struct TokenProb {
        int token;
        float prob;
    };

    static long long elapsed_us(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    }

    static double milliseconds(long long microseconds) noexcept {
        return static_cast<double>(microseconds) / 1000.0;
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

    static std::string format_timing(long long total_us, const PredictTiming& timing) {
        return std::format(
            "[TIME] predict_ms={:.3f} tokenize_ms={:.3f} cache_ms={:.3f} "
            "cache_batch_ms={:.3f} cache_decode_ms={:.3f} cache_sync_ms={:.3f} "
            "candidate_ms={:.3f} mask_ms={:.3f} step_batch_ms={:.3f} "
            "step_decode_ms={:.3f} step_sync_ms={:.3f} log_ms={:.3f} "
            "cache_tokens={} candidate_tokens={} mask_calls={} decode_calls={}",
            milliseconds(total_us), milliseconds(timing.tokenize_us),
            milliseconds(timing.cache_us), milliseconds(timing.cache_batch_us),
            milliseconds(timing.cache_decode_us), milliseconds(timing.cache_sync_us),
            milliseconds(timing.candidate_us), milliseconds(timing.mask_us),
            milliseconds(timing.step_batch_us), milliseconds(timing.step_decode_us),
            milliseconds(timing.step_sync_us), milliseconds(timing.log_us),
            timing.cache_tokens, timing.candidate_tokens, timing.mask_calls, timing.decode_calls);
    }

    void log_slow_decode(const char* stage, llama_pos pos, size_t token_count, llama_token last_token,
                         long long decode_us, long long sync_us) {
        const long long total_us = decode_us + sync_us;
        if (total_us < kSlowDecodeLogThresholdUs) return;

        const std::string stage_name(stage);
        logger_->log([stage_name, pos, token_count, last_token, decode_us, sync_us, total_us] {
            return std::format(
                "[TIME] slow_decode stage={} pos={} tokens={} last_token={} "
                "decode_ms={:.3f} sync_ms={:.3f} total_ms={:.3f}",
                stage_name, pos, token_count, last_token, milliseconds(decode_us),
                milliseconds(sync_us), milliseconds(total_us));
        });
    }

#if IME_CORE_TRACE_PREDICT
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

    void debug_request(const std::u16string& context, const std::vector<PaddingEntry>& padding,
                       const std::vector<int>& tokens) {
        logger_->log([context, padding, tokens] {
            std::string padding_text;
            for (const auto& entry : padding) padding_text += describe_padding(entry);
            std::string token_text;
            for (size_t i = 0; i < tokens.size(); ++i) {
                std::format_to(std::back_inserter(token_text), "{}{}", i == 0 ? "" : " ",
                               tokens[i]);
            }
            return std::format("[REQ] context=\"{}\" padding=\"{}\" tokens=[{}]",
                               to_utf8(context), padding_text, token_text);
        });
    }

    void debug_top5(size_t pos, const PaddingEntry& entry, const std::vector<TokenProb>& probs,
                    const std::map<llama_token, char32_t>& inv) {
        logger_->log([pos, entry, probs, inv] {
            std::string top5;
            const size_t count = std::min<size_t>(5, probs.size());
            for (size_t i = 0; i < count; ++i) {
                const auto token = static_cast<llama_token>(probs[i].token);
                const auto it = inv.find(token);
                const std::string word = (it == inv.end()) ? "?" : to_utf8(it->second);
                std::format_to(std::back_inserter(top5), "{}{}(token={}, p={:.6f})",
                               i == 0 ? "" : ", ", word, token, probs[i].prob);
            }
            return std::format("[POS {}] bpmf=\"{}\" top5={}", pos,
                               to_utf8(entry.bpmf), top5);
        });
    }

    void debug_no_candidates(size_t pos, const PaddingEntry& entry) {
        logger_->log([pos, entry] {
            return std::format("[POS {}] bpmf=\"{}\" top5=<no candidates>", pos,
                               to_utf8(entry.bpmf));
        });
    }

    void debug_chosen(size_t pos, const PaddingEntry& entry, int token) {
        logger_->log([pos, entry, token] {
            return std::format("[POS {}] chosen=\"{}\" token={}", pos,
                               to_utf8(entry.chosen_char), token);
        });
    }
#else
    void debug_request(const std::u16string&, const std::vector<PaddingEntry>&, const std::vector<int>&) {}

    void debug_top5(size_t, const PaddingEntry&, const std::vector<TokenProb>&,
                    const std::map<llama_token, char32_t>&) {}

    void debug_no_candidates(size_t, const PaddingEntry&) {}

    void debug_chosen(size_t, const PaddingEntry&, int) {}
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

#if IME_CORE_TRACE_PREDICT
        const auto previous_count = prev_tokens.size();
        const auto current_count = new_tokens.size();
        logger_->log([previous_count, current_count, common] {
            return std::format("[CORE] cache prev={} new={} common={}", previous_count,
                               current_count, common);
        });
#endif

        if (common < prev_tokens.size()) {
            bool ok = llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1);
#if IME_CORE_TRACE_PREDICT
            logger_->log([common, ok] {
                return std::format("[CORE] seq_rm from={} result={}", common,
                                   ok ? "ok" : "FAIL");
            });
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

}  // namespace llavon::ime::core::internal
