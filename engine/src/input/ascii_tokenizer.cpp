#include "input/ascii_tokenizer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace ime::fcitx5 {

namespace {

bool is_letter(char16_t ch) {
    return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z');
}

bool is_digit(char16_t ch) {
    return ch >= u'0' && ch <= u'9';
}

bool is_alnum(char16_t ch) {
    return is_letter(ch) || is_digit(ch);
}

// [a-zA-Z]+
size_t scan_letters(std::u16string_view raw, size_t begin) {
    size_t end = begin;
    while (end < raw.size() && is_letter(raw[end])) ++end;
    return end;
}

// [a-zA-Z0-9]+
size_t scan_alnum(std::u16string_view raw, size_t begin) {
    size_t end = begin;
    while (end < raw.size() && is_alnum(raw[end])) ++end;
    return end;
}

// [0-9]+(\.[0-9]+)* — plain integers, decimals, dotted versions and IPv4.
size_t scan_number(std::u16string_view raw, size_t begin) {
    size_t end = begin;
    while (end < raw.size() && is_digit(raw[end])) ++end;
    while (end < raw.size() && raw[end] == u'.' && end + 1 < raw.size() && is_digit(raw[end + 1])) {
        end += 2;
        while (end < raw.size() && is_digit(raw[end])) ++end;
    }
    return end;
}

// [a-zA-Z_][a-zA-Z0-9_]*
size_t scan_identifier(std::u16string_view raw, size_t begin) {
    if (begin >= raw.size()) return begin;
    const char16_t first = raw[begin];
    if (!is_letter(first) && first != u'_') return begin;
    size_t end = begin + 1;
    while (end < raw.size() && (is_alnum(raw[end]) || raw[end] == u'_')) ++end;
    return end;
}

// Longest letters-only prefix of [begin, end) with at least two characters.
std::optional<size_t> letters_only_prefix(std::u16string_view raw, size_t begin, size_t end) {
    size_t k = begin;
    while (k < end && is_letter(raw[k])) ++k;
    if (k - begin < 2) return std::nullopt;
    return k;
}

// label(.label)+ where every label starts with an alphanumeric (not a hyphen)
// and the final label ends in a letters-only TLD with at least two characters.
// Returns the longest valid domain; a trailing non-letter suffix on the final
// label (for example "gmail.com283") truncates the domain instead of failing.
std::optional<size_t> scan_domain(std::u16string_view raw, size_t begin) {
    if (begin >= raw.size() || !is_alnum(raw[begin])) return std::nullopt;

    size_t label_begin = begin;
    size_t label_end = begin;
    size_t last_dot = std::u16string_view::npos;
    std::optional<size_t> result;
    while (label_end < raw.size()) {
        while (label_end < raw.size() && (is_alnum(raw[label_end]) || raw[label_end] == u'-')) {
            if (raw[label_begin] == u'-') return result;
            ++label_end;
        }
        if (label_end == label_begin) return result;

        if (last_dot != std::u16string_view::npos) {
            if (const auto tld = letters_only_prefix(raw, label_begin, label_end)) result = *tld;
        }

        if (label_end < raw.size() && raw[label_end] == u'.') {
            last_dot = label_end;
            label_begin = label_end + 1;
            ++label_end;
            continue;
        }
        break;
    }
    return result;
}

// local@domain; returns the end of the email or std::nullopt.
std::optional<size_t> scan_email(std::u16string_view raw, size_t begin) {
    if (begin >= raw.size() || !is_alnum(raw[begin])) return std::nullopt;
    size_t at = begin;
    while (at < raw.size() && (is_alnum(raw[at]) || raw[at] == u'.' || raw[at] == u'_' || raw[at] == u'%' ||
                               raw[at] == u'+' || raw[at] == u'-')) {
        ++at;
    }
    if (at == begin || at >= raw.size() || raw[at] != u'@') return std::nullopt;

    const auto domain_end = scan_domain(raw, at + 1);
    if (!domain_end) return std::nullopt;
    return *domain_end;
}

// scheme://rest; emits the full URL plus a prefix ending at each '/' that
// follows the authority.
std::vector<AsciiToken> scan_url(std::u16string_view raw, size_t begin) {
    std::vector<AsciiToken> tokens;
    if (begin >= raw.size() || !is_letter(raw[begin])) return tokens;

    size_t scheme = begin + 1;
    while (scheme < raw.size() && (is_letter(raw[scheme]) || is_digit(raw[scheme]) || raw[scheme] == u'+' ||
                                   raw[scheme] == u'-' || raw[scheme] == u'.')) {
        ++scheme;
    }
    if (scheme + 2 >= raw.size() || raw[scheme] != u':' || raw[scheme + 1] != u'/' || raw[scheme + 2] != u'/') {
        return tokens;
    }

    size_t end = scheme + 3;
    while (end < raw.size() && raw[end] != u' ') ++end;

    tokens.push_back({AsciiTokenKind::URL, begin, end});
    // A prefix ending right after the authority domain ("https://example.com").
    if (const auto domain = scan_domain(raw, scheme + 3)) {
        tokens.push_back({AsciiTokenKind::URL, begin, *domain});
    }
    // A prefix ending at every path separator after the scheme.
    for (size_t i = scheme + 3; i < end; ++i) {
        if (raw[i] == u'/') {
            tokens.push_back({AsciiTokenKind::URL, begin, i});
        }
    }
    return tokens;
}

// /component/component... plus a prefix ending at every component boundary.
std::vector<AsciiToken> scan_filesystem_path(std::u16string_view raw, size_t begin) {
    std::vector<AsciiToken> tokens;
    if (begin >= raw.size() || raw[begin] != u'/') return tokens;

    size_t end = begin + 1;
    bool in_component = false;
    while (end < raw.size()) {
        const char16_t ch = raw[end];
        if (ch == u'/') {
            if (!in_component) return tokens;
            in_component = false;
            ++end;
            continue;
        }
        if (is_alnum(ch) || ch == u'.' || ch == u'-' || ch == u'_') {
            in_component = true;
            ++end;
            continue;
        }
        break;
    }
    if (!in_component && end == begin + 1) return tokens;

    tokens.push_back({AsciiTokenKind::FilesystemPath, begin, end});
    for (size_t i = begin + 1; i < end; ++i) {
        if (raw[i] == u'/' && i > begin + 1) {
            tokens.push_back({AsciiTokenKind::FilesystemPath, begin, i + 1});
        }
    }
    return tokens;
}

void push_token(std::vector<AsciiToken>& tokens, AsciiTokenKind kind, size_t begin, size_t end) {
    if (end <= begin) return;
    if (std::any_of(tokens.begin(), tokens.end(),
                    [&](const AsciiToken& token) { return token.end == end; })) {
        return;
    }
    tokens.push_back({kind, begin, end});
}

}  // namespace

std::vector<AsciiToken> tokenize_ascii(std::u16string_view raw, size_t begin) {
    std::vector<AsciiToken> tokens;
    if (begin >= raw.size()) return tokens;

    const char16_t first = raw[begin];
    const AsciiTokenKind single_kind =
        is_letter(first) ? AsciiTokenKind::LatinWord
        : is_digit(first) ? AsciiTokenKind::Number
                          : AsciiTokenKind::OperatorOrSymbol;
    push_token(tokens, single_kind, begin, begin + 1);

    if (is_letter(first)) {
        const size_t letters = scan_letters(raw, begin);
        push_token(tokens, AsciiTokenKind::LatinWord, begin, letters);

        const size_t alnum = scan_alnum(raw, begin);
        if (alnum > letters) {
            bool has_letter = false;
            bool has_digit = false;
            for (size_t i = begin; i < alnum; ++i) {
                has_letter = has_letter || is_letter(raw[i]);
                has_digit = has_digit || is_digit(raw[i]);
            }
            if (has_letter && has_digit) push_token(tokens, AsciiTokenKind::Alphanumeric, begin, alnum);
        }

        const size_t identifier = scan_identifier(raw, begin);
        if (identifier > letters) push_token(tokens, AsciiTokenKind::Identifier, begin, identifier);

        const auto domain = scan_domain(raw, begin);
        if (domain) push_token(tokens, AsciiTokenKind::Domain, begin, *domain);

        const auto email = scan_email(raw, begin);
        if (email) push_token(tokens, AsciiTokenKind::Email, begin, *email);

        for (const auto& token : scan_url(raw, begin)) {
            push_token(tokens, token.kind, token.begin, token.end);
        }
    } else if (is_digit(first)) {
        const size_t digits = scan_alnum(raw, begin);
        bool has_letter = false;
        bool has_digit = false;
        for (size_t i = begin; i < digits; ++i) {
            has_letter = has_letter || is_letter(raw[i]);
            has_digit = has_digit || is_digit(raw[i]);
        }
        if (has_letter && has_digit) push_token(tokens, AsciiTokenKind::Alphanumeric, begin, digits);

        const size_t number = scan_number(raw, begin);
        if (number > begin) push_token(tokens, AsciiTokenKind::Number, begin, number);
    } else if (first == u'/') {
        for (const auto& token : scan_filesystem_path(raw, begin)) {
            push_token(tokens, token.kind, token.begin, token.end);
        }
    }

    if (tokens.empty()) {
        push_token(tokens, single_kind, begin, begin + 1);
    }
    return tokens;
}

}  // namespace ime::fcitx5
