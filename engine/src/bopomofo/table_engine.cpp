#include "bopomofo/table_engine.hpp"

#include "text/utf.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ime::fcitx5 {

TableEngine::TableEngine(std::filesystem::path table_path) {
    std::ifstream input(table_path);
    if (!input) throw std::runtime_error("failed to open table: " + table_path.string());

    const auto json = nlohmann::json::parse(input);
    for (const auto& [key, value] : json.items()) {
        std::vector<char32_t> candidates;
        for (const auto& item : value) {
            candidates.push_back(first_utf8_codepoint(item.get<std::string>()));
        }
        mapping_.emplace(utf8_to_u16(key), std::move(candidates));
    }
}

std::vector<char32_t> TableEngine::lookup(std::u16string_view bopomofo) const {
    const auto it = mapping_.find(std::u16string(bopomofo));
    if (it == mapping_.end()) return {};
    return it->second;
}

}  // namespace ime::fcitx5
