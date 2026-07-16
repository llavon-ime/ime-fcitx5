#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace imesvc::ml {

class TrainingTokenizer final {
public:
    explicit TrainingTokenizer(const std::filesystem::path& tables_directory) {
        chars_ = load(tables_directory / "tokens" / "chars.json");
        latin_ = load(tables_directory / "tokens" / "latin.json");
        special_ = load(tables_directory / "tokens" / "special_tokens.json");
        bopomofo_ = load(tables_directory / "tokens" / "bpmf.json");
        for (const auto required : {"<BOS>", "<SEP>", "<SP>", "<UNK>", "<LATIN>"}) {
            if (!special_.contains(required)) throw std::runtime_error("missing tokenizer special token");
        }
    }

    [[nodiscard]] std::vector<std::int64_t> encode_context(std::string_view utf8) const;
    [[nodiscard]] std::int64_t bopomofo_token(std::string_view reading) const {
        std::string token;
        token.reserve(reading.size() + 2U);
        token.push_back('<');
        token.append(reading);
        token.push_back('>');
        const auto it = bopomofo_.find(token);
        if (it == bopomofo_.end()) throw std::runtime_error("unknown Bopomofo token");
        return it->second;
    }
    [[nodiscard]] std::int64_t character_token(std::string_view utf8_scalar) const {
        const auto it = chars_.find(std::string(utf8_scalar));
        if (it == chars_.end()) throw std::runtime_error("target is absent from the tokenizer vocabulary");
        return it->second;
    }
    [[nodiscard]] std::int64_t special(std::string_view name) const { return special_.at(std::string(name)); }

private:
    static std::unordered_map<std::string, std::int64_t> load(const std::filesystem::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("open tokenizer table failed: " + path.string());
        const auto json = nlohmann::json::parse(input);
        if (!json.is_object()) throw std::runtime_error("invalid tokenizer table");
        std::unordered_map<std::string, std::int64_t> result;
        for (const auto& [key, value] : json.items()) {
            if (!value.is_number_integer()) throw std::runtime_error("invalid tokenizer ID");
            result.emplace(key, value.get<std::int64_t>());
        }
        return result;
    }
    std::unordered_map<std::string, std::int64_t> chars_, latin_, special_, bopomofo_;
};

}  // namespace imesvc::ml
