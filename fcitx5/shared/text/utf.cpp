#include "text/utf.hpp"

#include <stdexcept>

namespace ime::fcitx5 {

namespace {

void append_utf8(std::string& output, char32_t codepoint) {
    if ((codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
        throw std::runtime_error("invalid Unicode scalar value");
    }

    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3FU)));
    }
}

}  // namespace

std::u32string utf8_to_u32(std::string_view input) {
    std::u32string output;
    for (size_t i = 0; i < input.size();) {
        const auto byte = static_cast<unsigned char>(input[i]);
        char32_t codepoint = 0;
        size_t length = 0;

        if ((byte & 0x80U) == 0) {
            codepoint = byte;
            length = 1;
        } else if ((byte & 0xE0U) == 0xC0U) {
            codepoint = byte & 0x1FU;
            length = 2;
        } else if ((byte & 0xF0U) == 0xE0U) {
            codepoint = byte & 0x0FU;
            length = 3;
        } else if ((byte & 0xF8U) == 0xF0U) {
            codepoint = byte & 0x07U;
            length = 4;
        } else {
            throw std::runtime_error("invalid UTF-8 sequence");
        }

        if (i + length > input.size()) throw std::runtime_error("truncated UTF-8 sequence");
        for (size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(input[i + offset]);
            if ((continuation & 0xC0U) != 0x80U) throw std::runtime_error("invalid UTF-8 continuation byte");
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }

        if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) || (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint > 0x10FFFF) {
            throw std::runtime_error("invalid Unicode scalar value");
        }

        output.push_back(codepoint);
        i += length;
    }
    return output;
}

std::u16string utf8_to_u16(std::string_view input) {
    std::u16string output;
    for (char32_t codepoint : utf8_to_u32(input)) {
        if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000;
            output.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10U)));
            output.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FFU)));
        }
    }
    return output;
}

std::string u16_to_utf8(std::u16string_view input) {
    std::string output;
    for (size_t i = 0; i < input.size(); ++i) {
        char32_t codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (i + 1 >= input.size()) throw std::runtime_error("truncated UTF-16 surrogate pair");
            const char32_t low = input[++i];
            if (low < 0xDC00 || low > 0xDFFF) throw std::runtime_error("invalid UTF-16 surrogate pair");
            codepoint = 0x10000 + (((codepoint - 0xD800) << 10U) | (low - 0xDC00));
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            throw std::runtime_error("invalid UTF-16 surrogate pair");
        }
        append_utf8(output, codepoint);
    }
    return output;
}

std::string char32_to_utf8(char32_t codepoint) {
    if (codepoint == 0) return {};
    std::string output;
    append_utf8(output, codepoint);
    return output;
}

char32_t first_utf8_codepoint(std::string_view input) {
    const auto decoded = utf8_to_u32(input);
    if (decoded.empty()) throw std::runtime_error("empty UTF-8 string");
    return decoded.front();
}

}  // namespace ime::fcitx5
