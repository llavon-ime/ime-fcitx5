#pragma once

#include <utf8/cpp20.h>

#include <filesystem>
#include <rfl/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "../utils/core_paths.hpp"

namespace llavon::ime::core::internal {

class HanziMapEngine {
    std::unordered_map<std::u16string, std::vector<char32_t>> mapping;

public:
    explicit HanziMapEngine(const CorePaths& paths) {
        auto path = paths.bopomofo_table_path().string();
        auto result = rfl::json::load<std::unordered_map<std::string, std::vector<std::string>>>(path);
        auto temp = result.value();
        for (auto& [k, v] : temp) {
            auto key = utf8::utf8to16(k);
            std::vector<char32_t> wvec;
            for (const auto& item : v) {
                wvec.push_back(utf8::utf8to32(item)[0]);
            }
            mapping[key] = std::move(wvec);
        }
    }

    std::vector<char32_t> lookup_all(const std::u16string& bopomofo) const {
        if (mapping.contains(bopomofo)) return mapping.at(bopomofo);
        return {};
    }
};

}  // namespace llavon::ime::core::internal
