#pragma once

#include <utf8/cpp20.h>

#include <cstddef>
#include <filesystem>
#include <rfl/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "../utils/runtime_paths.hpp"

namespace imesvc {

struct PaddingEntry {
    bool is_chosen;
    char32_t chosen_char = 0;
    std::u16string bpmf;
};

class Tokenizer {
    std::unordered_map<std::string, int> char_table;
    std::unordered_map<std::string, int> latin_table;
    std::unordered_map<std::string, int> special_table;
    std::unordered_map<std::string, int> bpmf_table;

    static std::filesystem::path resolve_table_path(const char* filename) {
        return RuntimePaths::token_table_path(filename);
    }

    Tokenizer() {
        char_table =
            rfl::json::load<std::unordered_map<std::string, int>>(resolve_table_path("chars.json").string()).value();
        latin_table =
            rfl::json::load<std::unordered_map<std::string, int>>(resolve_table_path("latin.json").string()).value();
        special_table =
            rfl::json::load<std::unordered_map<std::string, int>>(resolve_table_path("special_tokens.json").string())
                .value();
        bpmf_table =
            rfl::json::load<std::unordered_map<std::string, int>>(resolve_table_path("bpmf.json").string()).value();
    }

    static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool is_digit(int c) { return (c >= '0' && c <= '9'); }
    static bool is_latin_char(int c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '_' || c == '+'; }
    static char to_lower(int c) { return (c >= 'A' && c <= 'Z') ? char(c ^ 0x20) : char(c); }

    static void remove_leading_unknown_context_tokens(std::vector<int>& context_tokens, int unknown_token) {
        size_t first_kept = 0;
        while (first_kept < context_tokens.size() && context_tokens[first_kept] == unknown_token) first_kept++;
        if (first_kept == 0) return;

        context_tokens.erase(context_tokens.begin(), context_tokens.begin() + static_cast<std::ptrdiff_t>(first_kept));
    }

public:
    static Tokenizer& instance() {
        static Tokenizer tokenizer;
        return tokenizer;
    }

    int map_char(char32_t c) {
        std::string s;
        utf8::append(c, s);
        if (char_table.contains(s)) return char_table.at(s);
        return -1;
    }

    std::vector<int> tokenize(const std::u16string& context16, const std::vector<PaddingEntry>& padding) {
        std::vector<int> res;
        res.push_back(special_table.at("<BOS>"));
        std::vector<int> context_tokens;
        std::string context8 = utf8::utf16to8(context16);
        std::u32string context = utf8::utf8to32(context8);
        for (size_t i = 0; i < context.size(); i++) {
            std::string s;
            utf8::append(context[i], s);
            if (context[i] == U' ') {
                context_tokens.push_back(special_table.at("<SP>"));
            } else if (char_table.contains(s)) {
                context_tokens.push_back(char_table.at(s));
            } else if (is_latin_char(context[i])) {
                std::string str;
                for (; i < context.size(); i++) {
                    if (is_latin_char(context[i])) {
                        str.push_back(to_lower(context[i]));
                    } else {
                        i--;
                        break;
                    }
                }
                if (latin_table.contains(str)) {
                    context_tokens.push_back(latin_table.at(str));
                } else {
                    context_tokens.push_back(special_table.at("<LATIN>"));
                }
            } else {
                context_tokens.push_back(special_table.at("<UNK>"));
            }
        }
        remove_leading_unknown_context_tokens(context_tokens, special_table.at("<UNK>"));
        res.insert(res.end(), context_tokens.begin(), context_tokens.end());
        for (auto& entry : padding) {
            if (entry.is_chosen) {
                std::string s;
                utf8::append(entry.chosen_char, s);
                if (char_table.contains(s)) {
                    res.push_back(char_table.at(s));
                } else {
                    res.push_back(special_table.at("<UNK>"));
                }
            } else {
                std::string s8 = utf8::utf16to8(entry.bpmf);
                s8 = "<" + s8 + ">";
                if (!bpmf_table.contains(s8)) {
                    throw std::logic_error("invalid bpmf: " + s8);
                }
                res.push_back(bpmf_table.at(s8));
            }
        }
        res.push_back(special_table.at("<SEP>"));
        return res;
    }
};

}  // namespace imesvc
