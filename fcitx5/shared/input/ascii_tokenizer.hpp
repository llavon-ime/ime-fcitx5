#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace ime::fcitx5 {

// Generic ASCII grammar classification. The tokenizer knows nothing about
// specific websites, domains, or English words; it only classifies spans of
// the pending raw input by structure.
enum class AsciiTokenKind {
    LatinWord,
    Alphanumeric,
    Email,
    Domain,
    URL,
    FilesystemPath,
    Identifier,
    Number,
    OperatorOrSymbol,
};

struct AsciiToken {
    AsciiTokenKind kind = AsciiTokenKind::OperatorOrSymbol;
    size_t begin = 0;
    size_t end = 0;
};

// Returns every meaningful token that starts exactly at `begin` inside raw.
// The returned tokens may overlap (the maximal token plus its structural
// prefixes); each token is a lossless cover of raw[begin..end).
//
// Grammar coverage:
//   LatinWord        [a-zA-Z]+
//   Alphanumeric     [a-zA-Z0-9]+ with both letters and digits
//   Email            local@domain, local = [a-zA-Z0-9._%+-]+
//   Domain           label(.label)+ with a letters-only TLD
//   URL              scheme://rest, plus prefixes at each path separator
//   FilesystemPath   /components... plus prefixes at each component boundary
//   Identifier       [a-zA-Z_][a-zA-Z0-9_]*
//   Number           digits with dotted decimals / IPv4 / ports
//   OperatorOrSymbol everything else, one character at a time
std::vector<AsciiToken> tokenize_ascii(std::u16string_view raw, size_t begin);

}  // namespace ime::fcitx5
