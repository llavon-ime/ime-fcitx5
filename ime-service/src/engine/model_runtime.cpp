#include "model_runtime.hpp"

#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace imesvc {

namespace {

void require_utf8_continuation(const std::string& value, std::size_t index) {
    if (index >= value.size() || (static_cast<unsigned char>(value[index]) & 0xc0U) != 0x80U) {
        throw std::runtime_error("invalid UTF-8 table value");
    }
}

}  // namespace

SharedModelRuntime::SharedModelRuntime(RuntimeConfig config) : config_(std::move(config)) {}

const RuntimeConfig& SharedModelRuntime::config() const noexcept {
    return config_;
}

void SharedModelRuntime::validate_configuration() const {
    if (config_.tables_dir.empty() || !std::filesystem::is_directory(config_.tables_dir)) {
        throw std::runtime_error("tables directory not found: " + config_.tables_dir.string());
    }
    const auto table_path = config_.tables_dir / "bopomofo_char.json";
    if (!std::filesystem::is_regular_file(table_path)) {
        throw std::runtime_error("bopomofo table not found: " + table_path.string());
    }
    if (!config_.model_path.empty() && !std::filesystem::is_regular_file(config_.model_path)) {
        throw std::runtime_error("model file not found: " + config_.model_path.string());
    }
    if (config_.context_length == 0 || config_.context_length > 384 || config_.threads == 0) {
        throw std::runtime_error("context length must be between 1 and the native 384-token limit; threads must be positive");
    }
}

void SharedModelRuntime::ensure_loaded() const {
    std::call_once(load_once_, [this]() {
        try {
            validate_configuration();
            load_tables();
            // The POSIX build deliberately keeps the model bridge optional.  A regular model
            // path is validated above; table-backed prediction remains useful when no model
            // was packaged, and the session state is still owned by this shared runtime.
            std::lock_guard lock(state_mutex_);
            loaded_ = true;
        } catch (const std::exception& error) {
            std::lock_guard lock(state_mutex_);
            load_error_ = error.what();
            throw;
        }
    });

    std::lock_guard lock(state_mutex_);
    if (!loaded_) throw std::runtime_error(load_error_.empty() ? "model runtime is unavailable" : load_error_);
}

bool SharedModelRuntime::loaded() const noexcept {
    std::lock_guard lock(state_mutex_);
    return loaded_;
}

std::string SharedModelRuntime::load_error() const {
    std::lock_guard lock(state_mutex_);
    return load_error_;
}

ModelRuntimeStatus SharedModelRuntime::status() const {
    std::lock_guard lock(state_mutex_);
    ModelRuntimeStatus result;
    result.tables_loaded = loaded_;
    result.inference_model_loaded = inference_model_loaded_;
    result.adapter_loaded = adapter_loaded_;
    result.active_adapter_version = adapter_generation_ ? adapter_generation_->version : std::string{};
    result.error = inference_load_error_.empty() ? load_error_ : inference_load_error_;
    return result;
}

void SharedModelRuntime::record_inference_loaded(bool adapter_loaded) noexcept {
    std::lock_guard lock(state_mutex_);
    inference_model_loaded_ = true;
    adapter_loaded_ = adapter_loaded;
    inference_load_error_.clear();
}

void SharedModelRuntime::record_inference_failure(std::string error) noexcept {
    std::lock_guard lock(state_mutex_);
    inference_model_loaded_ = false;
    adapter_loaded_ = false;
    inference_load_error_ = std::move(error);
}

std::vector<char32_t> SharedModelRuntime::lookup(std::u16string_view bopomofo) const {
    ensure_loaded();
    std::lock_guard lock(state_mutex_);
    const auto it = bopomofo_table_.find(std::u16string(bopomofo));
    if (it == bopomofo_table_.end()) return {};
    return it->second;
}

std::shared_ptr<const AdapterGeneration> SharedModelRuntime::adapter_generation() const {
    std::lock_guard lock(state_mutex_);
    return adapter_generation_;
}

std::string SharedModelRuntime::active_adapter_version() const {
    std::lock_guard lock(state_mutex_);
    return adapter_generation_ ? adapter_generation_->version : std::string{};
}

void SharedModelRuntime::activate_adapter(std::string version, std::filesystem::path path, std::string sha256) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || version.empty() || sha256.empty()) {
        throw std::runtime_error("refusing invalid published LoRA adapter");
    }
    auto generation = std::make_shared<AdapterGeneration>();
    generation->version = std::move(version);
    generation->path = std::filesystem::absolute(std::move(path));
    generation->sha256 = std::move(sha256);
    std::lock_guard lock(state_mutex_);
    generation->revision = next_adapter_revision_++;
    adapter_generation_ = std::move(generation);
    adapter_loaded_ = false;
}

void SharedModelRuntime::clear_active_adapter() noexcept {
    std::lock_guard lock(state_mutex_);
    adapter_generation_.reset();
    adapter_loaded_ = false;
}

void SharedModelRuntime::set_adapter_failure_handler(std::function<void(std::string)> handler) {
    std::lock_guard lock(state_mutex_);
    adapter_failure_handler_ = std::move(handler);
}

void SharedModelRuntime::reject_active_adapter(const std::string& version) {
    std::function<void(std::string)> handler;
    {
        std::lock_guard lock(state_mutex_);
        if (!adapter_generation_ || adapter_generation_->version != version) return;
        adapter_generation_.reset();
        adapter_loaded_ = false;
        handler = adapter_failure_handler_;
    }
    if (handler) {
        try {
            handler(version);
        } catch (...) {
        }
    }
}

void SharedModelRuntime::load_tables() const {
    const auto path = config_.tables_dir / "bopomofo_char.json";
    std::ifstream input(path);
    if (!input) throw std::runtime_error("failed to open bopomofo table: " + path.string());

    const auto json = nlohmann::json::parse(input);
    if (!json.is_object() || json.size() > protocol::kMaxRepeatedFields) {
        throw std::runtime_error("invalid bopomofo table object");
    }

    std::unordered_map<std::u16string, std::vector<char32_t>> table;
    table.reserve(json.size());
    for (const auto& [key, value] : json.items()) {
        if (!value.is_array() || value.size() > protocol::kMaxRepeatedFields) {
            throw std::runtime_error("invalid bopomofo table entry");
        }
        auto& candidates = table[utf8_to_utf16(key)];
        candidates.reserve(value.size());
        for (const auto& item : value) {
            if (!item.is_string()) throw std::runtime_error("invalid bopomofo candidate");
            const auto candidate = first_codepoint(item.get<std::string>());
            if (!protocol::valid_scalar(candidate) || candidate == 0) {
                throw std::runtime_error("invalid bopomofo candidate scalar");
            }
            candidates.push_back(candidate);
        }
    }

    std::lock_guard lock(state_mutex_);
    bopomofo_table_ = std::move(table);
}

std::u16string SharedModelRuntime::utf8_to_utf16(const std::string& value) {
    std::u16string result;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        if (first <= 0x7fU) {
            codepoint = first;
            length = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            codepoint = first & 0x1fU;
            length = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codepoint = first & 0x0fU;
            length = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            codepoint = first & 0x07U;
            length = 4;
        } else {
            throw std::runtime_error("invalid UTF-8 table key");
        }
        if (i + length > value.size()) throw std::runtime_error("truncated UTF-8 table key");
        for (std::size_t j = 1; j < length; ++j) {
            require_utf8_continuation(value, i + j);
            codepoint = (codepoint << 6U) | (static_cast<unsigned char>(value[i + j]) & 0x3fU);
        }
        if (!protocol::valid_scalar(static_cast<char32_t>(codepoint)) ||
            (length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U)) {
            throw std::runtime_error("invalid UTF-8 table scalar");
        }
        if (codepoint <= 0xffffU) {
            result.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000U;
            result.push_back(static_cast<char16_t>(0xd800U + (codepoint >> 10U)));
            result.push_back(static_cast<char16_t>(0xdc00U + (codepoint & 0x3ffU)));
        }
        i += length;
    }
    return result;
}

char32_t SharedModelRuntime::first_codepoint(const std::string& value) {
    const auto utf16 = utf8_to_utf16(value);
    if (utf16.empty()) throw std::runtime_error("empty UTF-8 candidate");
    const auto first = static_cast<std::uint16_t>(utf16[0]);
    if (first >= 0xd800U && first <= 0xdbffU) {
        if (utf16.size() < 2) throw std::runtime_error("truncated UTF-16 candidate");
        const auto second = static_cast<std::uint16_t>(utf16[1]);
        return static_cast<char32_t>(0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U));
    }
    return static_cast<char32_t>(first);
}

}  // namespace imesvc
