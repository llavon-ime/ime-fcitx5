#include "session_engine.hpp"

#include <stdexcept>

#ifndef IMESVC_HAS_LLAMA
#define IMESVC_HAS_LLAMA 0
#endif

#if IMESVC_HAS_LLAMA
#include "llamaEngine.hpp"
#endif

namespace imesvc {

#if IMESVC_HAS_LLAMA
class LlamaSessionEngine final : public ISessionEngine {
public:
    explicit LlamaSessionEngine(std::shared_ptr<SharedModelRuntime> runtime) : runtime_(std::move(runtime)) {
        if (!runtime_) throw std::invalid_argument("llama session engine requires a runtime");
        adapter_generation_ = runtime_->adapter_generation();
    }

    std::vector<std::vector<char32_t>> predict(const protocol::PredictRequest& request) override {
        if (!engine_) {
            try {
                engine_ = std::make_unique<LlamaEngine>(runtime_, adapter_generation_);
                runtime_->record_inference_loaded(adapter_generation_ != nullptr);
            } catch (const std::exception& error) {
                if (!adapter_generation_) {
                    runtime_->record_inference_failure(error.what());
                    throw;
                }
                // A published adapter can become unreadable after an external filesystem fault.
                // Drop the in-memory generation and keep inference available on the verified base model.
                runtime_->reject_active_adapter(adapter_generation_->version);
                try {
                    engine_ = std::make_unique<LlamaEngine>(runtime_);
                    adapter_generation_.reset();
                    runtime_->record_inference_loaded(false);
                } catch (const std::exception& error) {
                    runtime_->record_inference_failure(error.what());
                    throw;
                }
            }
        }
        std::vector<PaddingEntry> padding;
        padding.reserve(request.padding.size());
        for (const auto& entry : request.padding) padding.push_back(PaddingEntry{entry.chosen, entry.chosen_char, entry.bopomofo});
        const auto predictions = engine_->predict(request.context, padding);
        std::vector<std::vector<char32_t>> result;
        result.reserve(predictions.size());
        for (const auto& prediction : predictions) {
            std::vector<char32_t> candidates;
            candidates.reserve(prediction.candidates.size());
            for (const auto& candidate : prediction.candidates) candidates.push_back(candidate.first);
            result.push_back(std::move(candidates));
        }
        return result;
    }

    bool loaded() const noexcept override { return runtime_->loaded(); }
    std::string adapter_version() const override {
        return adapter_generation_ ? adapter_generation_->version : std::string{};
    }

private:
    std::shared_ptr<SharedModelRuntime> runtime_;
    std::shared_ptr<const AdapterGeneration> adapter_generation_;
    std::unique_ptr<LlamaEngine> engine_;
};
#endif

SessionEngine::SessionEngine(std::shared_ptr<SharedModelRuntime> runtime) : runtime_(std::move(runtime)) {
    if (!runtime_) throw std::invalid_argument("session engine requires a shared runtime");
}

std::vector<std::vector<char32_t>> SessionEngine::predict(const protocol::PredictRequest& request) {
    runtime_->ensure_loaded();

    std::vector<std::vector<char32_t>> result;
    result.reserve(request.padding.size());
    for (const auto& entry : request.padding) {
        if (entry.chosen) {
            if (entry.chosen_char == 0 || !protocol::valid_scalar(entry.chosen_char)) {
                throw std::invalid_argument("chosen padding contains an invalid character");
            }
            result.push_back({entry.chosen_char});
            previous_tokens_.push_back(entry.chosen_char);
        } else {
            result.push_back(runtime_->lookup(entry.bopomofo));
            if (!result.back().empty()) previous_tokens_.push_back(result.back().front());
        }
        ++next_position_;
    }
    return result;
}

bool SessionEngine::loaded() const noexcept {
    return runtime_->loaded();
}

std::unique_ptr<ISessionEngine> create_session_engine(std::shared_ptr<SharedModelRuntime> runtime) {
#if IMESVC_HAS_LLAMA
    if (runtime && !runtime->config().model_path.empty()) return std::make_unique<LlamaSessionEngine>(std::move(runtime));
#endif
    return std::make_unique<SessionEngine>(std::move(runtime));
}

std::uint64_t SessionEngine::next_position() const noexcept {
    return next_position_;
}

const std::vector<char32_t>& SessionEngine::previous_tokens() const noexcept {
    return previous_tokens_;
}

}  // namespace imesvc
