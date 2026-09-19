#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ime::fcitx5 {

class TableEngine {
public:
    explicit TableEngine(std::filesystem::path table_path);

    std::vector<char32_t> lookup(std::u16string_view bopomofo) const;

private:
    std::unordered_map<std::u16string, std::vector<char32_t>> mapping_;
};

}  // namespace ime::fcitx5
