#pragma once

#include <utf8/cpp20.h>

#include <filesystem>
#include <rfl/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "../utils/runtime_paths.hpp"

namespace imesvc {

class HanziMapEngine {
    std::unordered_map<std::u16string, std::vector<char32_t>> mapping;

    static std::filesystem::path resolve_table_path() {
        return RuntimePaths::bopomofo_table_path();
    }

public:
    static HanziMapEngine& instance() {
        static HanziMapEngine engine;
        return engine;
    }

    std::vector<char32_t> lookup_all(const std::u16string& bopomofo) {
        if (mapping.contains(bopomofo)) return mapping[bopomofo];
        return {};
    }

private:
    HanziMapEngine() {
        auto path = resolve_table_path().string();
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
};

}  // namespace imesvc
